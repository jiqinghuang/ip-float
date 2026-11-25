#include "diag.h"

// winsock2.h 必须先于 windows.h；windns.h 依赖 winsock 类型
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <windns.h>

#include <cwchar>
#include <string>
#include <vector>

#include "http.h"
#include "ipinfo.h"
#include "netinfo.h"
#include "probe.h"
#include "util.h"

namespace {

// v4 出口：ipv4.icanhazip.com 只有 A 记录，必定走 IPv4，返回纯文本 IP
const wchar_t* kUrlV4  = L"https://ipv4.icanhazip.com";
// v6 出口：只列 AAAA 记录的域名，必定走 IPv6，返回纯文本 IP。
// 用域名而不是 IPv6 字面量，绕开 WinHTTP 对字面量的缺陷；direct=true 绕过系统代理。
const wchar_t* kUrlV6[] = {
    L"https://ipv6.icanhazip.com",
    L"https://api6.ipify.org",
};
// 归属 / ASN / 时区偏移：ipwho.is 免费、无 key、HTTPS
const wchar_t* kUrlGeo = L"https://ipwho.is/";

// 探测缓存的显式容器。以前散在各函数里的 static 变量不可重置、不可测，
// 也不方便给「立即刷新」加 force；现在集中在这里。
struct ProbeCache {
    std::wstring v6Key;
    ExitProbe    v6Result;
    bool         v6Have = false;
    ULONGLONG    v6At   = 0;

    std::wstring geoIp, geoAsn, geoCountry, geoOrg, geoRegion, geoCity;
    int          geoOffset = kNoOffset;
    std::wstring dnsIp, dnsCountry, dnsOrg;
    std::wstring offsetIp;
    int          offsetValue = kNoOffset;
};

// 卡片自动刷新共用一份；--diag 用局部实例，互不污染。
ProbeCache g_cache;

std::wstring Trim(const std::wstring& s);

// 用 Akamai 的 whoami 反查「我这台机器的 DNS 解析器出口 IP」：
// whoami.akamai.net 的 A 记录是按查询来源应答的，返回的就是递归解析器的地址。
std::wstring QueryResolverIp(DNS_STATUS& status)
{
    DNS_RECORDW* rec = nullptr;
    // 不用 DNS_QUERY_BYPASS_CACHE：解析器地址很少变，走系统缓存能把每次 1.5 秒降到 0。
    // 真变了也有 TTL 兜底，而且它只影响「DNS 那一行」的展示。
    status = DnsQuery_W(L"whoami.akamai.net", DNS_TYPE_A,
                        0, nullptr, &rec, nullptr);
    if (status != 0 || !rec) return std::wstring();

    std::wstring ip;
    const ULONG addr = rec->Data.A.IpAddress;     // 网络字节序
    const unsigned char* p = (const unsigned char*)&addr;
    if (p[0] != 0) {
        wchar_t buf[24] = {};
        swprintf(buf, 24, L"%u.%u.%u.%u", (unsigned)p[0], (unsigned)p[1],
                 (unsigned)p[2], (unsigned)p[3]);
        ip = buf;
    }
    DnsRecordListFree(rec, DnsFreeRecordList);
    return ip;
}

// v6 只测「能不能走通」：WinHTTP 对 IPv6 字面量支持有缺陷
// （带括号 connect 成功但 send 报 12029，不带括号报 12005），
// 所以这里直接开 TCP —— 我们关心的是隧道有没有承载 v6，不是拿响应内容。
bool ProbeV6Tcp(const wchar_t* addr, int port, int timeoutMs)
{
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;

    bool ok = false;
    SOCKET s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        sockaddr_in6 sa = {};
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons((u_short)port);
        IN6_ADDR a = {};
        if (InetPtonW(AF_INET6, addr, &a) == 1) {
            memcpy(&sa.sin6_addr, &a, sizeof(a));
            u_long nb = 1;
            ioctlsocket(s, FIONBIO, &nb);      // 非阻塞 + select 超时
            connect(s, (sockaddr*)&sa, sizeof(sa));
            fd_set wr, ex;
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            FD_SET(s, &wr);
            FD_SET(s, &ex);
            timeval tv = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
            int r = select(0, nullptr, &wr, &ex, &tv);
            ok = (r > 0 && FD_ISSET(s, &wr) && !FD_ISSET(s, &ex));
        }
        closesocket(s);
    }
    WSACleanup();
    return ok;
}

bool IsIpv6Text(const std::wstring& s)
{
    if (s.find(L':') == std::wstring::npos) return false;
    IN6_ADDR addr = {};
    return InetPtonW(AF_INET6, s.c_str(), &addr) == 1;
}

// 公网 IP 回显：默认严格校验证书；只有在证书类错误时才降级重试一次。
// 降级结果会在诊断里标明「证书未验证」，避免把 MITM 的地址当成真实出口。
bool HttpGetIpEcho(const std::wstring& url, std::string& body, std::wstring& err,
                   int timeoutMs, bool& tlsLenient)
{
    tlsLenient = false;
    if (HttpGet(url, body, err, timeoutMs, /*direct=*/true, /*lenientTls=*/false))
        return true;
    if (!IsTlsCertErrorText(err)) return false;
    if (!HttpGet(url, body, err, timeoutMs, /*direct=*/true, /*lenientTls=*/true))
        return false;
    tlsLenient = true;
    return true;
}

// 优先取真实 IPv6 出口地址；取不到时退回原来的 TCP 连通测试。
// 这两者要区分：有地址才能回答「v6 与 v4 是否同源」，只有连通性只能说「可走通」。
// key 描述「v6 出口这件事有没有可能变」：出口 IP + 隧道是否接管 v6 + 本机是否有公网 v6。
// 只要这三样没变，上一轮的结论就仍然成立 —— 而失败一次要 4~8 秒（WinHTTP 的超时
// 在 TUN 这条路上根本不生效，设 1.2s 实测能跑 7.5s），所以绝不每轮都去撞。
// v6 出口的归属：只有拿到地址时才查，按地址缓存 —— 一次要 1~3 秒，不能每轮重来。
void FillIpGeo(const std::wstring& ip, ExitProbe& out, ProbeCache& c)
{
    if (!c.geoIp.empty() && c.geoIp == ip) {
        out.asn = c.geoAsn;
        out.country = c.geoCountry;
        out.org = c.geoOrg;
        out.region = c.geoRegion;
        out.city = c.geoCity;
        if (out.utcOffsetMinutes == kNoOffset) out.utcOffsetMinutes = c.geoOffset;
        return;
    }

    std::string body;
    std::wstring err;
    if (!HttpGet(L"https://ipwho.is/" + ip, body, err, 3000)) return;

    bool found = false;
    long long n = JsonInt(body, "connection.asn", found);
    if (!found) n = JsonInt(body, "asn", found);
    std::wstring asn = (found && n > 0) ? (L"AS" + std::to_wstring(n)) : std::wstring();
    std::wstring cc = Utf8ToWide(JsonStr(body, "country_code"));
    if (cc.empty()) cc = Utf8ToWide(JsonStr(body, "country"));
    std::wstring org = Utf8ToWide(JsonStr(body, "connection.org"));
    if (org.empty()) org = Utf8ToWide(JsonStr(body, "org"));
    if (org.empty()) org = Utf8ToWide(JsonStr(body, "isp"));
    if (asn.empty() && cc.empty()) return;      // 什么都查不到，别写缓存

    out.asn = asn;
    out.country = cc;
    out.org = org;
    out.region = Utf8ToWide(JsonStr(body, "region"));
    out.city = Utf8ToWide(JsonStr(body, "city"));
    if (out.utcOffsetMinutes == kNoOffset) {
        long long off = JsonInt(body, "timezone.offset", found);
        if (!found) off = JsonInt(body, "offset", found);
        if (found) out.utcOffsetMinutes = (int)(off / 60);
    }
    c.geoIp = ip;
    c.geoAsn = asn;
    c.geoCountry = cc;
    c.geoOrg = org;
    c.geoRegion = out.region;
    c.geoCity = out.city;
    c.geoOffset = out.utcOffsetMinutes;
}

bool ProbeV6Exit(ExitProbe& out, int budgetMs, const std::wstring& key, bool force,
                 ProbeCache& c)
{
    // 失败也缓存，但只信 2 分钟：瞬时失败不能靠「点刷新」才恢复。
    // 成功（含地址/归属）10 分钟内复用；用户显式刷新时 force 绕过。
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG ttl = c.v6Result.ok ? 10ULL * 60 * 1000 : 2ULL * 60 * 1000;
    if (!force && c.v6Have && key == c.v6Key && now - c.v6At < ttl) {
        out = c.v6Result;
        return out.ok;
    }

    out.ok = false;
    out.ip.clear();
    out.err.clear();
    out.tlsLenient = false;
    if (budgetMs <= 0) budgetMs = 2000;

    // 用「总预算」而不是「每个 URL 各自超时」：以前两个回显各给 2.5 秒，
    // 而 WinHTTP 的一次失败实际要 4~8 秒，于是一轮白等十几秒。
    const ULONGLONG t0 = GetTickCount64();
    auto remaining = [&]() -> int {
        const ULONGLONG used = GetTickCount64() - t0;
        return used >= (ULONGLONG)budgetMs ? 0 : (int)(budgetMs - used);
    };

    for (const wchar_t* url : kUrlV6) {
        const int left = remaining();
        if (left <= 0) break;

        std::string body;
        std::wstring err;
        bool lenient = false;
        if (HttpGetIpEcho(url, body, err, left, lenient)) {
            const std::wstring ip = Trim(Utf8ToWide(body));
            if (IsIpv6Text(ip)) {
                out.ok = true;
                out.tlsLenient = lenient;
                out.ip = ip;
                FillIpGeo(ip, out, c);       // 归属决定「是否与 v4 同源」，必须有
                c.v6Key = key;
                c.v6Result = out;
                c.v6At = GetTickCount64();
                c.v6Have = true;
                return true;
            }
            out.err = L"响应不是 IPv6 地址";
        } else {
            out.err = err;
        }

        // 慢失败 = 这条路被黑洞吞了。换一个同类回显只会同样慢，直接放弃。
        if ((int)(GetTickCount64() - t0) >= budgetMs / 2) break;
    }

    // 兜底：只测连通性（拿不到地址，但能区分「完全不通」和「能连但没回显」）。
    // 注意 TUN 会在本地接住连接，所以这个结论只能说明"本地可达"。
    const int left = remaining();
    if (left > 0 && ProbeV6Tcp(L"2606:4700:4700::1111", 443, left < 1200 ? left : 1200)) {
        out.ok = true;
        out.ip.clear();
        out.err.clear();
        return true;
    }
    if (out.err.empty()) out.err = L"TCP 连接失败";

    c.v6Key = key;
    c.v6Result = out;
    c.v6At = GetTickCount64();
    c.v6Have = true;
    return false;
}

// 主数据源返回 IPv6 时，v4 出口要单独补一次。只用于诊断，不影响卡片首屏。
bool ProbeV4Exit(ExitProbe& out, int budgetMs, ProbeCache& c)
{
    out = ExitProbe();
    if (budgetMs <= 0) budgetMs = 2500;

    std::string body;
    std::wstring err;
    bool lenient = false;
    if (!HttpGetIpEcho(kUrlV4, body, err, budgetMs, lenient)) {
        out.err = err;
        return false;
    }
    const std::wstring ip = Trim(Utf8ToWide(body));
    if (ClassifyIp(ip) != IpKind::V4) {
        out.err = ip.empty() ? L"响应为空" : L"响应不是 IPv4 地址";
        return false;
    }

    out.ok = true;
    out.ip = ip;
    out.tlsLenient = lenient;
    FillIpGeo(ip, out, c);
    return true;
}

// DNS 解析器出口：一次本地 DNS 查询；解析器不在出口时再查一次它的归属。
// DnsQuery 的状态码翻成人话：光给数字没人知道该不该管
std::wstring DnsErrorText(long code)
{
    switch (code) {
        case 9501: return L"解析器返回了空应答（9501）—— 常见于刚切换 TUN 的瞬间";
        case 9003: return L"域名不存在（9003）";
        case 9002: return L"解析器故障（9002）";
        case 1460: return L"查询超时（1460）";
        default:   return L"错误码 " + std::to_wstring(code);
    }
}

void ProbeDns(ProbeInput& out, ProbeCache& c)
{
    // 这一项失败就整行变灰，代价太高。而且失败常发生在"刚切网络"的瞬间：
    // 那时解析器会返回空应答（9501），立刻重试会撞上同一个坏状态，所以要间隔一下。
    DNS_STATUS st = 0;
    std::wstring rip;
    for (int attempt = 0; attempt < 3 && rip.empty(); ++attempt) {
        if (attempt) Sleep(250);
        rip = QueryResolverIp(st);
    }

    if (rip.empty()) {
        out.dnsChecked = true;                 // 查了，只是没查到 —— 不是"跳过"
        out.dnsError = L"DnsQuery 无结果：" + DnsErrorText((long)st);
        return;
    }

    out.dnsChecked = true;
    out.dnsResolverIp = rip;
    if (rip == out.v4.ip) return;

    // 解析器地址很少变，查一次就够了：每次重查要多花 2~4 秒，失败时更久。
    if (!c.dnsIp.empty() && c.dnsIp == rip) {
        out.dnsResolverCountry = c.dnsCountry;
        out.dnsResolverOrg = c.dnsOrg;
        return;
    }

    std::string body;
    std::wstring err;
    if (!HttpGet(L"https://ipwho.is/" + rip, body, err, 3000)) return;
    out.dnsResolverCountry = Utf8ToWide(JsonStr(body, "country_code"));
    if (out.dnsResolverCountry.empty())
        out.dnsResolverCountry = Utf8ToWide(JsonStr(body, "country"));
    out.dnsResolverOrg = Utf8ToWide(JsonStr(body, "connection.org"));
    if (out.dnsResolverOrg.empty()) out.dnsResolverOrg = Utf8ToWide(JsonStr(body, "org"));
    if (out.dnsResolverOrg.empty()) out.dnsResolverOrg = Utf8ToWide(JsonStr(body, "isp"));

    if (!out.dnsResolverCountry.empty()) {
        c.dnsIp = rip;
        c.dnsCountry = out.dnsResolverCountry;
        c.dnsOrg = out.dnsResolverOrg;
    }
}

std::wstring Trim(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

// ipwho.is：geo + connection(asn/org) + timezone.offset（秒）
bool FillGeo(const std::string& json, ExitProbe& e)
{
    // 优先取国家码：DNS 那一行比的是国家码，两边必须同一套口径，
    // 否则「China」和「CN」会被判成不同国。
    e.country = Utf8ToWide(JsonStr(json, "country_code"));
    if (e.country.empty()) e.country = Utf8ToWide(JsonStr(json, "country"));
    e.region = Utf8ToWide(JsonStr(json, "region"));
    e.city   = Utf8ToWide(JsonStr(json, "city"));

    std::wstring org = Utf8ToWide(JsonStr(json, "connection.org"));
    if (org.empty()) org = Utf8ToWide(JsonStr(json, "org"));
    if (org.empty()) org = Utf8ToWide(JsonStr(json, "isp"));
    e.org = org;

    bool found = false;
    long long asn = JsonInt(json, "connection.asn", found);
    if (!found) asn = JsonInt(json, "asn", found);
    if (found && asn > 0) e.asn = L"AS" + std::to_wstring(asn);

    long long off = JsonInt(json, "timezone.offset", found);
    if (!found) off = JsonInt(json, "offset", found);
    if (found) e.utcOffsetMinutes = (int)(off / 60);

    return !e.country.empty();
}

bool WriteFileUtf8(const std::wstring& path, const std::wstring& text, bool bom)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    std::string utf8;
    if (bom) utf8.append("\xEF\xBB\xBF");
    utf8 += WideToUtf8(text);

    DWORD written = 0;
    BOOL ok = WriteFile(f, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
    CloseHandle(f);
    return ok && written == utf8.size();
}

std::wstring SplitPaths(const std::wstring& in, std::wstring& jsonPath)
{
    std::wstring txt = in;
    const std::wstring ext = L".txt";
    if (txt.size() > ext.size() &&
        _wcsicmp(txt.c_str() + (txt.size() - ext.size()), ext.c_str()) == 0) {
        jsonPath = txt.substr(0, txt.size() - ext.size()) + L".json";
    } else {
        jsonPath = txt + L".json";
        txt += L".txt";
    }
    return txt;
}

const wchar_t* SevW(Sev s)
{
    switch (s) {
        case Sev::Ok:   return L"OK";
        case Sev::Warn: return L"WARN";
        case Sev::Bad:  return L"BAD";
        default:        return L"--";
    }
}

std::wstring JsonEsc(const std::wstring& s)
{
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'"' || c == L'\\') { o += L'\\'; o += c; }
        else if (c == L'\n') o += L"\\n";
        else o += c;
    }
    return o;
}

std::wstring Pad(const std::wstring& s, size_t n)
{
    std::wstring o = s;
    while (o.size() < n) o += L' ';
    return o;
}

void ReplaceAll(std::wstring& s, const std::wstring& from, const wchar_t* to)
{
    if (from.empty()) return;
    const size_t toLen = wcslen(to);
    size_t p = 0;
    while ((p = s.find(from, p)) != std::wstring::npos) {
        s.replace(p, from.size(), to);
        p += toLen;
    }
}

// 只替换已知的、可能出现在报告里的敏感值；不改变任何判定结果。
void RedactText(std::wstring& s, const ProbeInput& in, const NetFacts& f)
{
    // 长地址先换：publicV6 可能以 v6 出口地址为前缀
    ReplaceAll(s, f.publicV6, L"<LOCAL_V6>");
    ReplaceAll(s, in.v6.ip, L"<IPv6>");
    ReplaceAll(s, in.v4.ip, L"<IPv4>");
    ReplaceAll(s, in.dnsResolverIp, L"<DNS>");
    ReplaceAll(s, f.lanIp, L"<LAN>");
    ReplaceAll(s, f.proxyServer, L"<PROXY>");
    ReplaceAll(s, f.tunIfName, L"<ADAPTER>");
    ReplaceAll(s, f.tunAdapter, L"<ADAPTER>");
    ReplaceAll(s, f.lanAdapter, L"<ADAPTER>");
}

void RedactDiagnosis(Diagnosis& d, const ProbeInput& in, const NetFacts& f)
{
    RedactText(d.summary, in, f);
    RedactText(d.evidence, in, f);
    for (auto& line : d.lines) {
        RedactText(line.value, in, f);
        RedactText(line.note, in, f);
    }
}

} // namespace

bool ProbeExits(const IpInfo& base, ProbeInput& out,
                const std::wstring& expectedCountry, const std::wstring& expectedAsn,
                bool force)
{
    out = ProbeInput();
    out.expectedCountry = expectedCountry;
    out.expectedAsn = expectedAsn;
    CollectNetFacts(out.facts);

    const IpKind baseKind = ClassifyIp(base.ip);
    if (baseKind == IpKind::V4) {
        out.v4.ok      = true;
        out.v4.ip      = base.ip;
        out.v4.country = base.country;
        out.v4.region  = base.region;
        out.v4.city    = base.city;
        out.v4.asn     = base.asn;
        out.v4.org     = base.org;
        out.v4.utcOffsetMinutes = base.utcOffsetMinutes;
    } else if (baseKind == IpKind::V6) {
        // 主源走了 IPv6：卡片显示这个地址没问题，但不能把它冒充成 IPv4 出口。
        // 诊断要的 v4 出口单独补一次；v6 直接用主源结果，避免再撞一次慢探测。
        out.v4.err = L"主数据源返回 IPv6，正在单独探测 IPv4";
        ProbeV4Exit(out.v4, 2500, g_cache);

        out.v6.ok = true;
        out.v6.ip = base.ip;
        out.v6.country = base.country;
        out.v6.region = base.region;
        out.v6.city = base.city;
        out.v6.asn = base.asn;
        out.v6.org = base.org;
        out.v6.utcOffsetMinutes = base.utcOffsetMinutes;
        if (out.v6.asn.empty() || out.v6.country.empty())
            FillIpGeo(out.v6.ip, out.v6, g_cache);
    } else {
        out.v4.err = L"没有可用的出口 IP";
    }

    // 时区偏移：数据源没给就问 ipwho.is（免费、无 key）。
    // 只取 offset / org，不覆盖已有的国家城市——那个 IP 可能是另一协议族的。
    // 时区偏移只跟出口 IP 有关，同一个出口不必每轮重查（每次 1~3 秒）。
    if (out.v4.ok && out.v4.utcOffsetMinutes == kNoOffset &&
        !g_cache.offsetIp.empty() && g_cache.offsetIp == out.v4.ip) {
        out.v4.utcOffsetMinutes = g_cache.offsetValue;
    }

    // 主源本来就是 v4 时才用 kUrlGeo 补偏移；v6 主源的 v4 由 ProbeV4Exit
    // 查具体 IP，不能拿当前出口（可能是 v6）的偏移去套。
    if (baseKind == IpKind::V4 && out.v4.ok && out.v4.utcOffsetMinutes == kNoOffset) {
        std::string body;
        std::wstring err;
        if (HttpGet(kUrlGeo, body, err, 3000)) {
            bool found = false;
            long long off = JsonInt(body, "timezone.offset", found);
            if (!found) off = JsonInt(body, "offset", found);
            if (found) {
                out.v4.utcOffsetMinutes = (int)(off / 60);
                g_cache.offsetIp = out.v4.ip;
                g_cache.offsetValue = out.v4.utcOffsetMinutes;
            }
            if (out.v4.org.empty()) {
                out.v4.org = Utf8ToWide(JsonStr(body, "connection.org"));
                if (out.v4.org.empty()) out.v4.org = Utf8ToWide(JsonStr(body, "org"));
            }
            if (out.v4.asn.empty()) {
                long long asn = JsonInt(body, "connection.asn", found);
                if (!found) asn = JsonInt(body, "asn", found);
                if (found && asn > 0) out.v4.asn = L"AS" + std::to_wstring(asn);
            }
        }
    }

    // v6 出口：主源是 v4 时才需要单独探；主源是 v6 的话上面已经用上了。
    if (baseKind == IpKind::V4) {
        ProbeV6Exit(out.v6, 2500,
                    out.v4.ip + L"|" + (out.facts.tunDefaultV6 ? L"1" : L"0") +
                    (out.facts.hasPublicV6 ? L"1" : L"0"), force, g_cache);
    }

    ProbeDns(out, g_cache);

    return true;
}

bool RunDiag(const std::wstring& outPath, bool redact, DiagFailOn failOn,
             std::wstring& err, int& exitCode)
{
    exitCode = 0;
    if (outPath.empty()) {
        err = L"--diag 需要一个输出路径";
        return false;
    }

    NetFacts facts;
    // 采集失败也照样出报告：部分数据比没有数据有用
    const bool factsOk = CollectNetFacts(facts);

    ProbeInput in;
    in.facts = facts;

    std::string body;
    std::wstring herr;

    // ── v4 出口 ──────────────────────────────────────────────────────
    bool v4Lenient = false;
    if (HttpGetIpEcho(kUrlV4, body, herr, 0, v4Lenient)) {
        in.v4.ip = Trim(Utf8ToWide(body));
        in.v4.ok = !in.v4.ip.empty();
        in.v4.tlsLenient = v4Lenient;
        if (!in.v4.ok) in.v4.err = L"响应为空";
    } else {
        in.v4.err = herr;
    }

    // ── 归属 / ASN / 时区偏移 ────────────────────────────────────────
    body.clear();
    herr.clear();
    if (HttpGet(kUrlGeo, body, herr)) {
        FillGeo(body, in.v4);
    } else if (in.v4.err.empty()) {
        in.v4.err = herr;
    }

    // ── v6 出口 ──────────────────────────────────────────────────────
    ProbeCache localCache;
    ProbeV6Exit(in.v6, 3000,
                in.v4.ip + L"|" + (in.facts.tunDefaultV6 ? L"1" : L"0") +
                (in.facts.hasPublicV6 ? L"1" : L"0"), /*force=*/true, localCache);

    ProbeDns(in, localCache);

    Diagnosis d = Diagnose(in);
    std::vector<Badge> badges = BuildBadges(d);

    // 判定用原始数据；展示/落盘用脱敏副本，避免把 LAN IP / 公网 IPv6 贴进 issue。
    Diagnosis reported = d;
    if (redact) RedactDiagnosis(reported, in, facts);

    // ── 文本报告 ──────────────────────────────────────────────────────
    std::wstring t;
    t += L"ip-float 网络诊断\n";
    t += L"生成时间 " + LocalTimeString(NowFileTime()) + L"\n";
    if (redact) t += L"已脱敏：IP / 网卡名 / 代理地址已替换为占位符\n";
    t += L"\n";
    if (!factsOk) t += L"采集本机信息失败：" + facts.err + L"\n\n";
    t += L"结论 [" + std::wstring(SevW(reported.verdict)) + L"] " + reported.summary + L"\n";
    t += L"依据 " + reported.evidence + L"\n\n";

    for (const auto& l : reported.lines) {
        std::wstring tag = std::wstring(L"[") + SevW(l.sev) + L"]";
        t += Pad(tag, 7) + Pad(l.label, 12) + l.value + L"\n";
        if (!l.note.empty()) t += std::wstring(19, L' ') + l.note + L"\n";
    }

    t += L"\n徽章 ";
    for (size_t i = 0; i < badges.size(); ++i) {
        if (i) t += L"   ";
        t += badges[i].label + L"=" + SevW(badges[i].sev);
    }
    t += L"\n";

    // ── 原始事实（方便对照，不参与判定）──────────────────────────────
    t += L"\n── 原始事实 ──\n";
    t += L"物理网卡    " + (facts.lanAdapter.empty() ? std::wstring(L"?") : facts.lanAdapter) +
         L"  " + facts.lanIp + L"\n";
    t += L"系统代理    ProxyEnable=" + std::wstring(facts.proxyEnabled ? L"1" : L"0") +
         L"  " + (facts.proxyServer.empty() ? std::wstring(L"(无)") : facts.proxyServer) +
         L"  PAC=" + (facts.pacConfigured ? L"yes" : L"no") + L"\n";
    t += L"隧道 v4     " + std::wstring(facts.tunDefaultV4 ? L"已接管" : L"未接管") +
         L"  " + facts.tunIfName + L"\n";
    t += L"隧道 v6     " + std::wstring(facts.tunDefaultV6 ? L"已接管" : L"未接管") + L"\n";
    t += L"本机 IPv6   " + (facts.hasPublicV6 ? facts.publicV6 : std::wstring(L"无")) + L"\n";
    t += L"本机时区    UTC" + std::wstring(facts.localOffsetMinutes >= 0 ? L"+" : L"") +
         std::to_wstring(facts.localOffsetMinutes / 60) + L"h  " + facts.localTzName + L"\n";
    t += L"定位服务    " + std::wstring(facts.locationConsent == 1 ? L"开启"
                                        : facts.locationConsent == 0 ? L"关闭" : L"未知") + L"\n";
    if (!in.v4.err.empty()) t += L"v4 探测     " + in.v4.err + L"\n";
    if (!in.v6.ok) t += L"v6 探测     " + in.v6.err + L"\n";

    if (redact) RedactText(t, in, facts);

    std::wstring jsonPath;
    std::wstring txtPath = SplitPaths(outPath, jsonPath);

    if (!WriteFileUtf8(txtPath, t, true)) {
        err = L"写文件失败：" + txtPath;
        return false;
    }

    std::wstring j = L"{\n";
    j += L"  \"verdict\": \"" + std::wstring(SevW(reported.verdict)) + L"\",\n";
    j += L"  \"summary\": \"" + JsonEsc(reported.summary) + L"\",\n";
    j += L"  \"evidence\": \"" + JsonEsc(reported.evidence) + L"\",\n";
    j += L"  \"redacted\": " + std::wstring(redact ? L"true" : L"false") + L",\n";
    j += L"  \"exitV4\": \"" + JsonEsc(redact ? std::wstring(L"<IPv4>") : in.v4.ip) + L"\",\n";
    j += L"  \"exitV6\": \"" + JsonEsc(redact ? std::wstring(L"<IPv6>") : in.v6.ip) + L"\",\n";
    j += L"  \"v4TlsLenient\": " + std::wstring(in.v4.tlsLenient ? L"true" : L"false") + L",\n";
    j += L"  \"v6TlsLenient\": " + std::wstring(in.v6.tlsLenient ? L"true" : L"false") + L",\n";
    j += L"  \"tunV4\": " + std::wstring(facts.tunDefaultV4 ? L"true" : L"false") + L",\n";
    j += L"  \"tunV6\": " + std::wstring(facts.tunDefaultV6 ? L"true" : L"false") + L",\n";
    j += L"  \"hasPublicV6\": " + std::wstring(facts.hasPublicV6 ? L"true" : L"false") + L",\n";
    j += L"  \"proxyEnabled\": " + std::wstring(facts.proxyEnabled ? L"true" : L"false") + L",\n";
    j += L"  \"badges\": [";
    for (size_t i = 0; i < badges.size(); ++i) {
        if (i) j += L", ";
        j += L"{\"label\": \"" + JsonEsc(badges[i].label) + L"\", \"sev\": \"" +
             SevW(badges[i].sev) + L"\"}";
    }
    j += L"]\n}\n";

    if (!WriteFileUtf8(jsonPath, j, false)) {
        err = L"写文件失败：" + jsonPath;
        return false;
    }

    if (failOn == DiagFailOn::Bad && d.verdict == Sev::Bad) {
        exitCode = 2;
    } else if (failOn == DiagFailOn::Warn &&
               (d.verdict == Sev::Bad || d.verdict == Sev::Warn)) {
        exitCode = 2;
    }
    return true;
}
