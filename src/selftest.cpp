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
    return RenderCardToPng(path, m, t, err);
}

} // namespace selftest
