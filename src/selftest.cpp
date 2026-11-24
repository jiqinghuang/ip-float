#include "selftest.h"

#include "card.h"
#include "util.h"

namespace selftest {

DemoCard MakeDemoCard(uint64_t now)
{
    DemoCard d;
    d.info.ip = L"203.0.113.42";
    d.info.country = L"JP";
    d.info.region = L"Tokyo";
    d.info.city = L"Chiyoda";
    d.info.asn = L"AS20473";
    d.info.org = L"The Constant Company, LLC";
    d.info.source = L"https://ipinfo.io/json";

    uint64_t at = now;
    FileTimeMinusSeconds(now, 130, at);   // 让自检图像「2 分钟前」，而不是刚刷新
    d.updatedAt = at;
    d.info.at = at;
    return d;
}

bool RenderDemoCard(const std::wstring& path, float scale, std::wstring& err)
{
    uint64_t now = NowFileTime();
    DemoCard d = MakeDemoCard(now);
    CardMetrics m = ComputeCardMetrics(scale);
    CardText t = BuildCardText(d.info, true, false, false, L"", d.updatedAt, now);

    // 演示数据走一遍真实判定，让 --render 的图和实际卡片长得一样（网站素材靠它）
    ProbeInput in;
    in.facts.tunDefaultV4 = true;
    in.facts.tunDefaultV6 = true;
    in.facts.tunIfName = L"Wintun";
    in.facts.hasPublicV6 = true;
    in.facts.publicV6 = L"2001:db8::1";
    in.facts.localOffsetMinutes = 540;
    in.facts.proxyConfigured = false;
    in.v4.ok = true;
    in.v4.ip = d.info.ip;
    in.v4.country = d.info.country;
    in.v4.utcOffsetMinutes = 540;
    in.v6.ok = true;
    in.v6.ip = d.info.ip;
    in.dnsChecked = true;
    in.dnsResolverIp = L"1.1.1.1";
    in.dnsResolverCountry = L"US";      // 和示例出口同国，徽章才是绿的
    in.dnsResolverOrg = L"Cloudflare";

    Diagnosis diag = Diagnose(in);
    t.badges = BuildBadges(diag);
    if (diag.verdict != Sev::Bad && diag.verdict != Sev::Skip)
        t.dot = diag.verdict == Sev::Warn ? DotState::Warn : DotState::Ok;

    return RenderCardToPng(path, m, t, err);
}

} // namespace selftest
