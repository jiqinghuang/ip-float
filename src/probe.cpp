#include "probe.h"

#include <cwchar>
#include <cwctype>

namespace {

int Rank(Sev s)
{
    switch (s) {
        case Sev::Bad:  return 3;
        case Sev::Warn: return 2;
        case Sev::Ok:   return 1;
        default:        return 0;   // Skip 不参与总判定
    }
}

Sev Worst(Sev a, Sev b) { return Rank(b) > Rank(a) ? b : a; }

// 公共 anycast 解析器的属地经常和出口国对不上，不能当成「漏」
bool IsWellKnownResolver(const std::wstring& org)
{
    static const wchar_t* kNames[] = {
        L"Cloudflare", L"Google", L"Quad9", L"OpenDNS", L"Cisco",
        L"AdGuard", L"NextDNS", L"Hurricane Electric", L"Neustar"
    };
    for (const wchar_t* n : kNames)
        if (org.find(n) != std::wstring::npos) return true;
    return false;
}

// 480 -> "+8"，-300 -> "-5"，330 -> "+5:30"
std::wstring HoursText(int minutes)
{
    bool neg = minutes < 0;
    int m = neg ? -minutes : minutes;
    std::wstring s = neg ? L"-" : L"+";
    s += std::to_wstring(m / 60);
    if (m % 60) {
        s += L":";
        std::wstring mm = std::to_wstring(m % 60);
        if (mm.size() < 2) s += L"0";
        s += mm;
    }
    return s;
}

void Push(Diagnosis& d, LineId id, const wchar_t* label, std::wstring value, Sev sev,
          std::wstring note = std::wstring(), bool na = false)
{
    ProbeLine l;
    l.id = id;
    l.label = label;
    l.value = std::move(value);
    l.sev = sev;
    l.na = na;
    l.note = std::move(note);
    d.lines.push_back(std::move(l));
}

std::wstring TrimText(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool SameText(const std::wstring& a, const std::wstring& b)
{
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// 配置里允许写 "20473" 或 "as20473"，统一成 IpInfo.asn 的 "AS20473"。
std::wstring NormalizeAsnText(const std::wstring& raw)
{
    std::wstring s;
    for (wchar_t c : raw) {
        if (!iswspace(c)) s += (wchar_t)towupper(c);
    }
    if (s.empty()) return s;
    if (s.size() >= 2 && s[0] == L'A' && s[1] == L'S') return s;
    if (s[0] >= L'0' && s[0] <= L'9') return L"AS" + s;
    return s;
}

void PushExpectation(Diagnosis& d, const ProbeInput& in)
{
    const std::wstring wantCountry = TrimText(in.expectedCountry);
    const std::wstring wantAsn = NormalizeAsnText(in.expectedAsn);
    if (wantCountry.empty() && wantAsn.empty()) return;

    if (!in.v4.ok) {
        Push(d, LineId::Expect, L"期望", L"未知", Sev::Skip,
             L"IPv4 出口探测失败，无法核对");
        return;
    }

    const bool countryOk = wantCountry.empty() || SameText(wantCountry, in.v4.country);
    const bool asnOk = wantAsn.empty() || SameText(wantAsn, in.v4.asn);
    std::wstring want = wantCountry;
    if (!wantAsn.empty()) want += (want.empty() ? L"" : L" / ") + wantAsn;
    std::wstring got = in.v4.country;
    if (!in.v4.asn.empty()) got += (got.empty() ? L"" : L" / ") + in.v4.asn;
    if (got.empty()) got = L"未知";

    if (countryOk && asnOk) {
        Push(d, LineId::Expect, L"期望", L"符合", Sev::Ok, L"配置 " + want);
    } else {
        Push(d, LineId::Expect, L"期望", L"不符", Sev::Warn,
             L"期望 " + want + L"，实际 " + got);
    }
}

} // namespace

Diagnosis Diagnose(const ProbeInput& in)
{
    Diagnosis d;
    const NetFacts&  f  = in.facts;
    const ExitProbe& v4 = in.v4;
    const ExitProbe& v6 = in.v6;

    const bool tunnelInUse = f.tunDefaultV4;
    const bool proxyInUse  = f.proxyEnabled;

    // ── 隧道 ──────────────────────────────────────────────────────────
    if (!tunnelInUse) {
        Push(d, LineId::Tunnel, L"隧道",
             f.tunAdapter.empty() ? L"未使用" : L"存在但未接管",
             Sev::Skip,
             f.tunAdapter.empty() ? L"默认路由仍走物理网卡，当前直连" : f.tunAdapter,
             /*na=*/true);
    } else if (f.hasPublicV6 && !f.tunDefaultV6) {
        Push(d, LineId::Tunnel, L"隧道", L"已接管 IPv4，IPv6 未接管", Sev::Warn, f.tunIfName);
    } else {
        Push(d, LineId::Tunnel, L"隧道",
             f.tunDefaultV6 ? L"已接管 IPv4 + IPv6" : L"已接管 IPv4", Sev::Ok, f.tunIfName);
    }

    // ── IPv6 路由（这是最容易被忽略的泄漏面）──────────────────────────
    if (!f.hasPublicV6 && !f.tunDefaultV6) {
        Push(d, LineId::V6Route, L"IPv6 路由", L"本机无公网 IPv6", Sev::Skip,
             std::wstring(), /*na=*/true);
    } else if (!tunnelInUse && !proxyInUse) {
        // 没开任何代理/隧道：v6 直连就是正常上网，不构成「泄漏」
        Push(d, LineId::V6Route, L"IPv6 路由", L"直连", Sev::Skip,
             L"未使用隧道或代理，不存在泄漏问题", /*na=*/true);
    } else if (f.hasPublicV6 && !f.tunDefaultV6) {
        // 有人在用隧道/代理，却留着一条直连的公网 v6 —— 结构性泄漏
        Sev sev = tunnelInUse ? Sev::Bad : Sev::Warn;
        std::wstring note = f.publicV6.empty() ? L"IPv6 流量会绕过隧道"
                                               : (L"真实地址 " + f.publicV6 + L" 会直接暴露");
        Push(d, LineId::V6Route, L"IPv6 路由", L"公网 IPv6 未走隧道", sev, note);
    } else {
        Push(d, LineId::V6Route, L"IPv6 路由", L"已接管", Sev::Ok);
    }

    // ── 出口路径 ──────────────────────────────────────────────────────
    // 这一行回答的不是「系统代理开关是几」，而是「我现在靠什么出去、有几条路」。
    // 只报开关的话，四种组合里三种都是绿的，等于没有信息量。
    {
        // PAC 配了就算走代理：AutoConfigURL 的优先级高于 ProxyEnable
        const bool proxyOn = f.proxyEnabled || f.pacConfigured;
        const std::wstring proxyDesc = f.pacConfigured ? std::wstring(L"PAC 脚本")
                                                       : f.proxyServer;

        if (tunnelInUse && proxyOn) {
            Push(d, LineId::Proxy, L"出口路径", L"TUN + 系统代理", Sev::Warn,
                 proxyDesc + L"：两条路并存，冗余，且会让 DNS 行为分裂");
        } else if (tunnelInUse) {
            Push(d, LineId::Proxy, L"出口路径", L"TUN", Sev::Ok,
                 L"全部流量走隧道；系统代理已关闭");
        } else if (proxyOn) {
            Push(d, LineId::Proxy, L"出口路径", L"系统代理", Sev::Ok,
                 proxyDesc + L"：当前唯一出口（TUN 未接管）");
        } else {
            // 没有任何代理路径 —— 「会不会漏」这个问题不成立，不是「通过」
            Push(d, LineId::Proxy, L"出口路径", L"直连", Sev::Skip,
                 L"没有配置任何代理，也没有隧道；不涉及泄漏判定", /*na=*/true);
        }
    }

    // ── IPv4 出口 ────────────────────────────────────────────────────
    if (!v4.ok) {
        Push(d, LineId::ExitV4, L"IPv4 出口", L"探测失败", Sev::Bad, v4.err);
    } else {
        std::wstring note = v4.country;
        if (!v4.region.empty()) note += (note.empty() ? L"" : L" ") + v4.region;
        if (!v4.city.empty())   note += (note.empty() ? L"" : L" ") + v4.city;
        Push(d, LineId::ExitV4, L"IPv4 出口", v4.ip, Sev::Ok, note);
    }

    // ── IPv6 出口 ────────────────────────────────────────────────────
    if (!v6.ok) {
        std::wstring note = f.hasPublicV6
            ? L"本机有公网 IPv6，但 v6 目的地不可达（被墙或运营商 v6 路由异常）"
            : L"v6 目标打不通；本机无公网 v6 时属正常";
        Push(d, LineId::ExitV6, L"IPv6 出口", L"不可达", Sev::Skip, note);
    } else if (v6.ip.empty()) {
        // 连通性有了，但远端回显拿不到地址。不要把「可走通」冒充成出口地址，
        // 也不要把本机公网 IPv6 写进「出口」这一行；本机地址单独列一行。
        Push(d, LineId::ExitV6, L"IPv6 出口", L"未取到", Sev::Skip,
             L"远端回显不可用，无法确认 IPv6 出口地址");
        if (f.hasPublicV6 && !f.publicV6.empty()) {
            Push(d, LineId::V6Local, L"本机 IPv6", f.publicV6, Sev::Skip,
                 L"本机公网 IPv6，不是远端出口；只有 IPv6 路由未接管时才会直接暴露");
        }
    } else if (v4.ok && !v6.ip.empty() && v6.ip != v4.ip) {
        Push(d, LineId::ExitV6, L"IPv6 出口", v6.ip, Sev::Bad,
             L"与 IPv4 出口 " + v4.ip + L" 不同源");
    } else if (v4.ok) {
        Push(d, LineId::ExitV6, L"IPv6 出口", L"与 IPv4 同源", Sev::Ok, v6.ip);
    } else {
        Push(d, LineId::ExitV6, L"IPv6 出口", v6.ip, Sev::Skip);
    }

    // ── 归属 / 类型 ──────────────────────────────────────────────────
    if (v4.ok) {
        std::wstring v = v4.asn;
        if (!v4.org.empty()) v += (v.empty() ? L"" : L" ") + v4.org;
        const wchar_t* kind = v4.isHosting ? L"机房 IP"
                            : (v4.isProxy || v4.isVpn) ? L"代理 / VPN"
                            : L"类型未知（数据源未提供）";
        Push(d, LineId::Asn, L"归属", v.empty() ? L"未知" : v, Sev::Ok, kind);
    } else {
        Push(d, LineId::Asn, L"归属", L"未知", Sev::Skip);
    }

    // ── 期望出口（可选配置）──────────────────────────────────────────
    PushExpectation(d, in);

    // ── 时区一致性 ───────────────────────────────────────────────────
    if (!v4.ok || v4.utcOffsetMinutes == kUnknownOffset) {
        Push(d, LineId::Tz, L"时区", L"未知", Sev::Skip, L"数据源未给出 UTC 偏移");
    } else if (v4.utcOffsetMinutes == f.localOffsetMinutes) {
        Push(d, LineId::Tz, L"时区", L"一致", Sev::Ok,
             L"IP " + HoursText(v4.utcOffsetMinutes) +
             L" / 本机 " + HoursText(f.localOffsetMinutes));
    } else {
        Push(d, LineId::Tz, L"时区", L"不一致", Sev::Warn,
             L"IP " + HoursText(v4.utcOffsetMinutes) +
             L" / 本机 " + HoursText(f.localOffsetMinutes));
    }

    // ── DNS ──────────────────────────────────────────────────────────
    if (!in.dnsChecked) {
        Push(d, LineId::Dns, L"DNS", L"未检测", Sev::Skip, L"这一轮没有执行");
    } else if (in.dnsResolverIp.empty()) {
        Push(d, LineId::Dns, L"DNS", L"查询失败", Sev::Skip,
             in.dnsError.empty() ? std::wstring(L"解析器地址查询失败") : in.dnsError);
    } else if (!v4.ok) {
        Push(d, LineId::Dns, L"DNS", in.dnsResolverIp, Sev::Skip);
    } else if (in.dnsResolverIp == v4.ip) {
        Push(d, LineId::Dns, L"DNS", L"与出口同源", Sev::Ok, in.dnsResolverIp);
    } else if (in.dnsResolverCountry.empty()) {
        Push(d, LineId::Dns, L"DNS", in.dnsResolverIp, Sev::Skip, L"无法查该解析器的归属");
    } else if (!v4.country.empty() && in.dnsResolverCountry == v4.country) {
        Push(d, LineId::Dns, L"DNS", L"与出口同国", Sev::Ok,
             in.dnsResolverIp + L"（" + in.dnsResolverCountry + L"）");
    } else if (IsWellKnownResolver(in.dnsResolverOrg)) {
        // Cloudflare / Google 这类是 anycast，属地常和出口对不上，不算矛盾
        Push(d, LineId::Dns, L"DNS", L"公共解析器", Sev::Ok,
             in.dnsResolverOrg + L"  " + in.dnsResolverIp);
    } else {
        Push(d, LineId::Dns, L"DNS", L"出口与 IP 不同国", Sev::Warn,
             in.dnsResolverIp + L"（" + in.dnsResolverCountry + L"）—— 查询可能没走隧道");
    }

    // ── 总判定 ───────────────────────────────────────────────────────
    for (const auto& l : d.lines) d.verdict = Worst(d.verdict, l.sev);

    if (!v4.ok) {
        // 探测失败不等于有泄漏：卡片会报红，但结论不能冒充「发现问题」
        d.summary = L"出口探测失败，无法判断";
    } else if (d.verdict == Sev::Bad) {
        d.summary = L"发现泄漏或出口不一致";
    } else if (d.verdict == Sev::Warn) {
        d.summary = L"存在可疑项";
    } else {
        d.summary = L"未发现泄漏";
    }

    if (v4.ok && v6.ok && !v4.ip.empty() && !v6.ip.empty()) {
        d.evidence = (v6.ip == v4.ip)
            ? (L"IPv4 / IPv6 出口同为 " + v4.ip)
            : (L"IPv4 出口 " + v4.ip + L"，IPv6 出口 " + v6.ip);
    } else if (v4.ok) {
        d.evidence = L"IPv4 出口 " + v4.ip;
        if (f.hasPublicV6 && !f.tunDefaultV6)
            d.evidence += L"；本机有公网 IPv6 但未走隧道";
    } else {
        d.evidence = L"出口探测失败：" + v4.err;
    }

    return d;
}

std::vector<Badge> BuildBadges(const Diagnosis& d)
{
    std::vector<Badge> out;
    out.reserve(5);

    auto add = [&](const wchar_t* label, LineId id) {
        Badge b;
        b.label = label;
        if (const ProbeLine* l = d.Find(id)) { b.sev = l->sev; b.na = l->na; }
        out.push_back(std::move(b));
    };

    add(L"TUN",  LineId::Tunnel);
    add(L"V6",   LineId::V6Route);
    add(L"路径", LineId::Proxy);
    add(L"时区", LineId::Tz);
    add(L"DNS",  LineId::Dns);
    return out;
}

const char* SevTag(Sev s)
{
    switch (s) {
        case Sev::Ok:   return "OK";
        case Sev::Warn: return "WARN";
        case Sev::Bad:  return "BAD";
        default:        return "--";
    }
}
