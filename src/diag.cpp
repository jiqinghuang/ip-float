#include "diag.h"

// winsock2.h 必须先于 windows.h；windns.h 依赖 winsock 类型
#include <winsock2.h>
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
// 归属 / ASN / 时区偏移：ipwho.is 免费、无 key、HTTPS
const wchar_t* kUrlGeo = L"https://ipwho.is/";
// v6 出口：用 IPv6 字面量直连 Cloudflare，绕过 DNS，纯粹测「v6 目的地能不能走通、出口是谁」。
// 1.1.1.1 的证书里有 2606:4700:4700::1111 的 IP SAN，TLS 校验没问题。
const wchar_t* kUrlV6  = L"https://[2606:4700:4700::1111]/cdn-cgi/trace";

// 用 Akamai 的 whoami 反查「我这台机器的 DNS 解析器出口 IP」：
// whoami.akamai.net 的 A 记录是按查询来源应答的，返回的就是递归解析器的地址。
std::wstring QueryResolverIp()
{
    DNS_RECORDW* rec = nullptr;
    const DNS_STATUS st = DnsQuery_W(L"whoami.akamai.net", DNS_TYPE_A,
                                     DNS_QUERY_BYPASS_CACHE, nullptr, &rec, nullptr);
    if (st != 0 || !rec) return std::wstring();

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

// DNS 解析器出口：一次本地 DNS 查询；解析器不在出口时再查一次它的归属。
void ProbeDns(ProbeInput& out)
{
    const std::wstring rip = QueryResolverIp();
    if (rip.empty()) return;

    out.dnsChecked = true;
    out.dnsResolverIp = rip;
    if (rip == out.v4.ip) return;

    std::string body;
    std::wstring err;
    if (!HttpGet(L"https://ipwho.is/" + rip, body, err, 3000)) return;
    out.dnsResolverCountry = Utf8ToWide(JsonStr(body, "country_code"));
    if (out.dnsResolverCountry.empty())
        out.dnsResolverCountry = Utf8ToWide(JsonStr(body, "country"));
    out.dnsResolverOrg = Utf8ToWide(JsonStr(body, "org"));
    if (out.dnsResolverOrg.empty())
        out.dnsResolverOrg = Utf8ToWide(JsonStr(body, "isp"));
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

// Cloudflare /cdn-cgi/trace：每行 "key=value"
bool ParseTrace(const std::string& body, ExitProbe& e)
{
    size_t p = 0;
    while (p < body.size()) {
        size_t nl = body.find('\n', p);
        std::string line = (nl == std::string::npos) ? body.substr(p) : body.substr(p, nl - p);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string k = line.substr(0, eq);
            std::string v = line.substr(eq + 1);
            if (k == "ip")       e.ip = Utf8ToWide(v);
            else if (k == "loc") e.country = Utf8ToWide(v);
        }
        if (nl == std::string::npos) break;
        p = nl + 1;
    }
    e.ok = !e.ip.empty();
    if (!e.ok) e.err = L"响应里没有 ip 字段";
    return e.ok;
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

bool ProbeExits(const IpInfo& base, ProbeInput& out)
{
    out = ProbeInput();
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
    if (out.v4.ok && out.v4.utcOffsetMinutes == kNoOffset) {
        std::string body;
        std::wstring err;
        if (HttpGet(kUrlGeo, body, err, 3000)) {
            bool found = false;
            long long off = JsonInt(body, "offset", found);
            if (found) out.v4.utcOffsetMinutes = (int)(off / 60);
            if (out.v4.org.empty()) out.v4.org = Utf8ToWide(JsonStr(body, "org"));
            if (out.v4.asn.empty()) {
                long long asn = JsonInt(body, "asn", found);
                if (found && asn > 0) out.v4.asn = L"AS" + std::to_wstring(asn);
            }
        }
    }

    // v6 出口：IP 字面量直连 CF（绕过 DNS），3s 超时——不能拖慢卡片刷新
    {
        std::string body;
        std::wstring err;
        if (HttpGet(kUrlV6, body, err, 3000)) ParseTrace(body, out.v6);
        else out.v6.err = err;
    }

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
    if (HttpGet(kUrlV4, body, herr)) {
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
    body.clear();
    herr.clear();
    if (HttpGet(kUrlV6, body, herr)) {
        ParseTrace(body, in.v6);
    } else {
        in.v6.ok = false;
        in.v6.err = herr;
    }

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
