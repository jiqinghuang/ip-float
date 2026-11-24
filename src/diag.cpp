#include "diag.h"

// winsock2.h 必须先于 windows.h；windns.h 依赖 winsock 类型
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h>
#include <windows.h>
#include <windns.h>

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

// 优先取真实 IPv6 出口地址；取不到时退回原来的 TCP 连通测试。
// 这两者要区分：有地址才能回答「v6 与 v4 是否同源」，只有连通性只能说「可走通」。
// key 描述「v6 出口这件事有没有可能变」：出口 IP + 隧道是否接管 v6 + 本机是否有公网 v6。
// 只要这三样没变，上一轮的结论就仍然成立 —— 而失败一次要 4~8 秒（WinHTTP 的超时
// 在 TUN 这条路上根本不生效，设 1.2s 实测能跑 7.5s），所以绝不每轮都去撞。
bool ProbeV6Exit(ExitProbe& out, int budgetMs, const std::wstring& key)
{
    static std::wstring cachedKey;
    static ExitProbe    cached;
    static bool         haveCache = false;

    if (haveCache && key == cachedKey) {
        out = cached;
        return out.ok;
    }

    out.ok = false;
    out.ip.clear();
    out.err.clear();
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
        if (HttpGet(url, body, err, left, /*direct=*/true, /*lenientTls=*/true)) {
            const std::wstring ip = Trim(Utf8ToWide(body));
            if (IsIpv6Text(ip)) {
                out.ok = true;
                out.ip = ip;
                cachedKey = key;
                cached = out;
                haveCache = true;
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

    cachedKey = key;
    cached = out;
    haveCache = true;
    return false;
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

void ProbeDns(ProbeInput& out)
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
    static std::wstring cachedIp, cachedCountry, cachedOrg;
    if (!cachedIp.empty() && cachedIp == rip) {
        out.dnsResolverCountry = cachedCountry;
        out.dnsResolverOrg = cachedOrg;
        return;
    }

    std::string body;
    std::wstring err;
    if (!HttpGet(L"https://ipwho.is/" + rip, body, err, 3000)) return;
    out.dnsResolverCountry = Utf8ToWide(JsonStr(body, "country_code"));
    if (out.dnsResolverCountry.empty())
        out.dnsResolverCountry = Utf8ToWide(JsonStr(body, "country"));
    out.dnsResolverOrg = Utf8ToWide(JsonStr(body, "org"));
    if (out.dnsResolverOrg.empty())
        out.dnsResolverOrg = Utf8ToWide(JsonStr(body, "isp"));

    if (!out.dnsResolverCountry.empty()) {
        cachedIp = rip;
        cachedCountry = out.dnsResolverCountry;
        cachedOrg = out.dnsResolverOrg;
    }
}

std::wstring Trim(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t' || s[a] == L'\r' || s[a] == L'\n')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t' || s[b - 1] == L'\r' || s[b - 1] == L'\n')) --b;
    return s.substr(a, b - a);
}

// JsonStr 只认带引号的字符串；数字字段（"asn": 17621、"offset": 28800）用这个
long long JsonInt(const std::string& json, const std::string& key, bool& found)
{
    found = false;
    const std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return 0;
    size_t i = pos + pat.size();
    auto skipWs = [&] {
        while (i < json.size() &&
               (json[i] == ' ' || json[i] == '\t' || json[i] == '\n' || json[i] == '\r')) ++i;
    };
    skipWs();
    if (i >= json.size() || json[i] != ':') return 0;
    ++i;
    skipWs();
    bool neg = false;
    if (i < json.size() && json[i] == '-') { neg = true; ++i; }
    if (i >= json.size() || json[i] < '0' || json[i] > '9') return 0;
    long long v = 0;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9') {
        v = v * 10 + (json[i] - '0');
        ++i;
    }
    found = true;
    return neg ? -v : v;
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

    std::wstring org = Utf8ToWide(JsonStr(json, "org"));
    if (org.empty()) org = Utf8ToWide(JsonStr(json, "isp"));
    e.org = org;

    bool found = false;
    long long asn = JsonInt(json, "asn", found);
    if (found && asn > 0) e.asn = L"AS" + std::to_wstring(asn);

    long long off = JsonInt(json, "offset", found);
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

} // namespace

bool ProbeExits(const IpInfo& base, ProbeInput& out,
                const std::wstring& expectedCountry, const std::wstring& expectedAsn)
{
    out = ProbeInput();
    out.expectedCountry = expectedCountry;
    out.expectedAsn = expectedAsn;
    CollectNetFacts(out.facts);

    out.v4.ok      = !base.ip.empty();
    out.v4.ip      = base.ip;
    out.v4.country = base.country;
    out.v4.region  = base.region;
    out.v4.city    = base.city;
    out.v4.asn     = base.asn;
    out.v4.org     = base.org;
    out.v4.utcOffsetMinutes = base.utcOffsetMinutes;
    if (!out.v4.ok) out.v4.err = L"没有可用的出口 IP";

    // 时区偏移：数据源没给就问 ipwho.is（免费、无 key）。
    // 只取 offset / org，不覆盖已有的国家城市——那个 IP 可能是另一协议族的。
    // 时区偏移只跟出口 IP 有关，同一个出口不必每轮重查（每次 1~3 秒）。
    static std::wstring offsetIp;
    static int          offsetValue = kNoOffset;
    if (out.v4.ok && out.v4.utcOffsetMinutes == kNoOffset &&
        !offsetIp.empty() && offsetIp == out.v4.ip) {
        out.v4.utcOffsetMinutes = offsetValue;
    }

    if (out.v4.ok && out.v4.utcOffsetMinutes == kNoOffset) {
        std::string body;
        std::wstring err;
        if (HttpGet(kUrlGeo, body, err, 3000)) {
            bool found = false;
            long long off = JsonInt(body, "offset", found);
            if (found) {
                out.v4.utcOffsetMinutes = (int)(off / 60);
                offsetIp = out.v4.ip;
                offsetValue = out.v4.utcOffsetMinutes;
            }
            if (out.v4.org.empty()) out.v4.org = Utf8ToWide(JsonStr(body, "org"));
            if (out.v4.asn.empty()) {
                long long asn = JsonInt(body, "asn", found);
                if (found && asn > 0) out.v4.asn = L"AS" + std::to_wstring(asn);
            }
        }
    }

    // v6 出口：优先取真实地址；取不到再退回连通性测试。
    // 2.5s 是后台诊断的超时，卡片 IP 不会被它阻塞（见 app.cpp 的两阶段抓取）。
    ProbeV6Exit(out.v6, 2500,
                out.v4.ip + L"|" + (out.facts.tunDefaultV6 ? L"1" : L"0") +
                (out.facts.hasPublicV6 ? L"1" : L"0"));

    ProbeDns(out);

    return true;
}

bool RunDiag(const std::wstring& outPath, std::wstring& err)
{
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
    if (HttpGet(kUrlV4, body, herr, 0, /*direct=*/true, /*lenientTls=*/true)) {
        in.v4.ip = Trim(Utf8ToWide(body));
        in.v4.ok = !in.v4.ip.empty();
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
    ProbeV6Exit(in.v6, 3000,
                in.v4.ip + L"|" + (in.facts.tunDefaultV6 ? L"1" : L"0") +
                (in.facts.hasPublicV6 ? L"1" : L"0"));

    ProbeDns(in);

    Diagnosis d = Diagnose(in);
    std::vector<Badge> badges = BuildBadges(d);

    // ── 文本报告 ──────────────────────────────────────────────────────
    std::wstring t;
    t += L"ip-float 网络诊断\n";
    t += L"生成时间 " + LocalTimeString(NowFileTime()) + L"\n\n";
    if (!factsOk) t += L"采集本机信息失败：" + facts.err + L"\n\n";
    t += L"结论 [" + std::wstring(SevW(d.verdict)) + L"] " + d.summary + L"\n";
    t += L"依据 " + d.evidence + L"\n\n";

    for (const auto& l : d.lines) {
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

    std::wstring jsonPath;
    std::wstring txtPath = SplitPaths(outPath, jsonPath);

    if (!WriteFileUtf8(txtPath, t, true)) {
        err = L"写文件失败：" + txtPath;
        return false;
    }

    std::wstring j = L"{\n";
    j += L"  \"verdict\": \"" + std::wstring(SevW(d.verdict)) + L"\",\n";
    j += L"  \"summary\": \"" + JsonEsc(d.summary) + L"\",\n";
    j += L"  \"evidence\": \"" + JsonEsc(d.evidence) + L"\",\n";
    j += L"  \"exitV4\": \"" + JsonEsc(in.v4.ip) + L"\",\n";
    j += L"  \"exitV6\": \"" + JsonEsc(in.v6.ip) + L"\",\n";
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

    WriteFileUtf8(jsonPath, j, false);
    return true;
}
