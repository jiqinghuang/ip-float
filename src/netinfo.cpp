#include "netinfo.h"

// winsock2.h 必须先于 windows.h，否则 ws2ipdef.h 会缺 SCOPE_ID/SOCKET_ADDRESS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>

#include <windows.h>
#include <iphlpapi.h>

#include <cwctype>
#include <string>
#include <vector>

namespace {

const wchar_t* kInetKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
const wchar_t* kLocKey  =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\location";

// MinGW 的 IP_ADAPTER_ADDRESSES::Description 是 PWCHAR；MSVC SDK 是 CHAR*。
// 这里只支持 MinGW（项目实际工具链），所以直接取宽字符版本。
inline std::wstring AdapterDesc(const IP_ADAPTER_ADDRESSES* a)
{
    return a->Description ? a->Description : L"";
}

bool ReadRegStr(HKEY root, const wchar_t* sub, const wchar_t* name, std::wstring& out)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    wchar_t buf[1024] = {};
    DWORD cb = sizeof(buf) - sizeof(wchar_t);
    DWORD type = 0;
    LONG rc = RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)buf, &cb);
    RegCloseKey(k);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
    out = buf;
    return true;
}

bool ReadRegDword(HKEY root, const wchar_t* sub, const wchar_t* name, DWORD& out)
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    DWORD cb = sizeof(DWORD), type = 0;
    LONG rc = RegQueryValueExW(k, name, nullptr, &type, (LPBYTE)&out, &cb);
    RegCloseKey(k);
    return rc == ERROR_SUCCESS && type == REG_DWORD;
}

std::wstring AddrToStr(const SOCKADDR_INET* a)
{
    if (!a) return L"";
    wchar_t buf[64] = {};
    if (a->si_family == AF_INET) {
        if (!InetNtopW(AF_INET, (void*)&a->Ipv4.sin_addr, buf, 64)) return L"";
    } else if (a->si_family == AF_INET6) {
        if (!InetNtopW(AF_INET6, (void*)&a->Ipv6.sin6_addr, buf, 64)) return L"";
    }
    return buf;
}

// 只看单播地址是不是「公网 IPv6」：排除 loopback / link-local / ULA(fc00::/7)
bool IsPublicV6(const SOCKADDR_INET* a, std::wstring& text)
{
    if (!a || a->si_family != AF_INET6) return false;
    const unsigned char* b = (const unsigned char*)&a->Ipv6.sin6_addr;
    bool allZero = true;
    for (int i = 0; i < 16; ++i) if (b[i]) { allZero = false; break; }
    if (allZero) return false;                          // ::
    if (b[0] == 0xFE && (b[1] & 0xC0) == 0x80) return false;   // fe80::/10 link-local
    if ((b[0] & 0xFE) == 0xFC) return false;            // fc00::/7 unique-local
    bool loopback = true;
    for (int i = 0; i < 15; ++i) if (b[i]) { loopback = false; break; }
    if (loopback && b[15] == 1) return false;           // ::1
    text = AddrToStr(a);
    return !text.empty();
}

struct Adapter {
    DWORD        if4 = 0;
    DWORD        if6 = 0;
    std::wstring desc;
    std::wstring friendly;
    bool         up = false;
    bool         hasGateway = false;
    bool         loopback = false;
    std::wstring ipv4;
    std::wstring publicV6;
};

bool LoadAdapters(std::vector<Adapter>& out, std::wstring& err)
{
    ULONG size = 16 * 1024;
    std::vector<BYTE> buf(size);
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST;

    for (int attempt = 0; attempt < 3; ++attempt) {
        ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                       (IP_ADAPTER_ADDRESSES*)buf.data(), &size);
        if (rc == ERROR_SUCCESS) break;
        if (rc != ERROR_BUFFER_OVERFLOW) {
            err = L"GetAdaptersAddresses 失败（" + std::to_wstring(rc) + L"）";
            return false;
        }
        buf.resize(size);
    }

    for (auto* a = (IP_ADAPTER_ADDRESSES*)buf.data(); a; a = a->Next) {
        Adapter ad;
        ad.if4 = a->IfIndex;
        ad.if6 = a->Ipv6IfIndex;
        ad.desc = AdapterDesc(a);
        ad.friendly = a->FriendlyName ? a->FriendlyName : L"";
        ad.up = (a->OperStatus == IfOperStatusUp);
        ad.loopback = (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK);
        ad.hasGateway = (a->FirstGatewayAddress != nullptr);

        for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
            auto* sa = (SOCKADDR_INET*)ua->Address.lpSockaddr;
            if (!sa) continue;
            if (sa->si_family == AF_INET && ad.ipv4.empty()) {
                std::wstring s = AddrToStr(sa);
                if (s != L"127.0.0.1") ad.ipv4 = s;
            } else if (sa->si_family == AF_INET6 && ad.publicV6.empty()) {
                std::wstring s;
                if (IsPublicV6(sa, s)) ad.publicV6 = s;
            }
        }
        out.push_back(std::move(ad));
    }
    return true;
}

bool BestRoute(const wchar_t* dest, WORD family, DWORD& ifIndex, std::wstring& nextHop)
{
    SOCKADDR_INET dst = {};
    dst.si_family = family;
    int ok = 0;
    if (family == AF_INET) {
        dst.Ipv4.sin_family = AF_INET;
        ok = InetPtonW(AF_INET, dest, &dst.Ipv4.sin_addr);
    } else {
        dst.Ipv6.sin6_family = AF_INET6;
        ok = InetPtonW(AF_INET6, dest, &dst.Ipv6.sin6_addr);
    }
    if (ok != 1) return false;

    MIB_IPFORWARD_ROW2 row = {};
    SOCKADDR_INET src = {};
    if (GetBestRoute2(nullptr, 0, nullptr, &dst, 0, &row, &src) != NO_ERROR) return false;
    ifIndex = row.InterfaceIndex;
    nextHop = AddrToStr(&row.NextHop);
    return true;
}

int ReadLocationConsent()
{
    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
        std::wstring v;
        if (!ReadRegStr(root, kLocKey, L"Value", v)) continue;
        if (_wcsicmp(v.c_str(), L"Allow") == 0) return 1;
        if (_wcsicmp(v.c_str(), L"Deny") == 0) return 0;
        return -1;
    }
    return -1;
}

} // namespace

bool CollectNetFacts(NetFacts& out)
{
    out = NetFacts();

    // ── 系统代理 ──────────────────────────────────────────────────────
    DWORD enable = 0;
    if (ReadRegDword(HKEY_CURRENT_USER, kInetKey, L"ProxyEnable", enable))
        out.proxyEnabled = (enable != 0);
    ReadRegStr(HKEY_CURRENT_USER, kInetKey, L"ProxyServer", out.proxyServer);
    std::wstring pac;
    if (ReadRegStr(HKEY_CURRENT_USER, kInetKey, L"AutoConfigURL", pac) && !pac.empty())
        out.pacConfigured = true;
    out.proxyConfigured = !out.proxyServer.empty() || out.pacConfigured;

    // ── 网卡 ──────────────────────────────────────────────────────────
    std::vector<Adapter> ads;
    std::wstring err;
    if (!LoadAdapters(ads, err)) {
        out.err = err;
        return false;
    }

    // 主物理网卡：UP、非回环、有默认网关
    const Adapter* primary = nullptr;
    for (const auto& a : ads) {
        if (!a.up || a.loopback || !a.hasGateway) continue;
        primary = &a;
        break;
    }
    if (primary) {
        out.lanIp = primary->ipv4;
        out.lanAdapter = primary->friendly;
    }

    // 全局 IPv6：优先取主网卡的
    if (primary && !primary->publicV6.empty()) {
        out.hasPublicV6 = true;
        out.publicV6 = primary->publicV6;
    } else {
        for (const auto& a : ads) {
            if (!a.up || a.loopback || a.publicV6.empty()) continue;
            out.hasPublicV6 = true;
            out.publicV6 = a.publicV6;
            break;
        }
    }

    // ── 隧道：默认路由是否落在物理网卡之外 ────────────────────────────
    auto ifName = [&](DWORD idx, bool v6) -> std::wstring {
        for (const auto& a : ads) {
            if ((v6 ? a.if6 : a.if4) == idx) return a.friendly.empty() ? a.desc : a.friendly;
        }
        return L"";
    };

    DWORD idx4 = 0, idx6 = 0;
    std::wstring hop4, hop6;
    if (BestRoute(L"1.1.1.1", AF_INET, idx4, hop4) && primary && primary->if4 != 0) {
        out.tunDefaultV4 = (idx4 != primary->if4);
        if (out.tunDefaultV4) {
            out.tunIfName = ifName(idx4, false);
            out.tunAdapter = out.tunIfName;
        }
    }
    if (BestRoute(L"2606:4700:4700::1111", AF_INET6, idx6, hop6) && primary && primary->if6 != 0) {
        out.tunDefaultV6 = (idx6 != primary->if6);
    }

    // ── 时区 ──────────────────────────────────────────────────────────
    TIME_ZONE_INFORMATION tzi = {};
    DWORD tzState = GetTimeZoneInformation(&tzi);
    int bias = (int)tzi.Bias;
    if (tzState == TIME_ZONE_ID_DAYLIGHT)      bias += (int)tzi.DaylightBias;
    else if (tzState == TIME_ZONE_ID_STANDARD) bias += (int)tzi.StandardBias;
    out.localOffsetMinutes = -bias;
    out.localTzName = tzi.StandardName;

    // ── 定位服务 ──────────────────────────────────────────────────────
    out.locationConsent = ReadLocationConsent();
    return true;
}
