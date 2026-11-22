// 纯逻辑单测：JSON 解析 / IpInfo / ShortAsn / Ago / URL host。
// 不建窗口、不联网；build.ps1 -Test 会编译并运行它。
#include "../src/ipinfo.h"
#include "../src/util.h"

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool cond, const char* what)
{
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        ++g_failures;
        printf("FAIL %s\n", what);
    }
}

void CheckStr(const std::string& got, const std::string& want, const char* what)
{
    if (got == want) {
        printf("ok   %s\n", what);
    } else {
        ++g_failures;
        printf("FAIL %s\n  got  = [%s]\n  want = [%s]\n",
               what, got.c_str(), want.c_str());
    }
}

void CheckW(const std::wstring& got, const std::wstring& want, const char* what)
{
    if (got == want) {
        printf("ok   %s\n", what);
    } else {
        ++g_failures;
        std::string g = WideToUtf8(got);
        std::string w = WideToUtf8(want);
        printf("FAIL %s\n  got  = [%s]\n  want = [%s]\n",
               what, g.c_str(), w.c_str());
    }
}

void TestJson()
{
    CheckStr(JsonStr(R"({"ip":"203.0.113.42"})", "ip"), "203.0.113.42", "json plain");
    CheckStr(JsonStr(R"({"city":"Foo \"Bar\""})", "city"), "Foo \"Bar\"", "json escaped quote");
    CheckStr(JsonStr(R"({"a":"x\r\n\t"})", "a"), "x\r\n\t", "json \\r\\n\\t");
    CheckStr(JsonStr(R"({"a":"x\b\f"})", "a"), "x\b\f", "json \\b\\f");

    CheckStr(JsonStr(R"({"city":"\u6771\u4EAC"})", "city"),
             WideToUtf8(L"東京"), "json \\u BMP");
    CheckStr(JsonStr(R"({"e":"\uD83D\uDE00"})", "e"),
             WideToUtf8(L"\U0001F600"), "json surrogate pair");
    CheckStr(JsonStr(R"({"a":"\u12"})", "a"), "\\u12", "json truncated \\u");
    CheckStr(JsonStr(R"({"a":"\uZZZZ"})", "a"), "\\uZZZZ", "json invalid \\u");

    // "country" 不能误命中 "country_code"
    CheckStr(JsonStr(R"({"country_code":"JP","country":"Japan"})", "country"),
             "Japan", "json exact key match");
}

void TestParse()
{
    IpInfo ipinfo;
    std::wstring err;
    bool ok = ParseIpInfo(
        R"({"ip":"203.0.113.42","city":"Chiyoda","region":"Tokyo","country":"JP",)"
        R"("org":"AS20473 The Constant Company, LLC"})",
        L"https://ipinfo.io/json", ipinfo, err);
    Check(ok, "parse ipinfo ok");
    CheckW(ipinfo.ip, L"203.0.113.42", "parse ipinfo ip");
    CheckW(ipinfo.country, L"JP", "parse ipinfo country");
    CheckW(ipinfo.region, L"Tokyo", "parse ipinfo region");
    CheckW(ipinfo.city, L"Chiyoda", "parse ipinfo city");
    CheckW(ipinfo.asn, L"AS20473", "parse ipinfo asn from org");
    CheckW(ipinfo.org, L"The Constant Company, LLC", "parse ipinfo org");
    CheckW(ipinfo.source, L"https://ipinfo.io/json", "parse ipinfo source");
    Check(ipinfo.at != 0, "parse ipinfo timestamp");

    IpInfo ipapi;
    ok = ParseIpInfo(
        R"({"ip":"198.51.100.7","country_code":"US","region":"California",)"
        R"("city":"Los Angeles","org":"Cloudflare, Inc.","asn":"AS13335"})",
        L"https://ipapi.co/json/", ipapi, err);
    Check(ok, "parse ipapi ok");
    CheckW(ipapi.country, L"US", "parse ipapi country_code");
    CheckW(ipapi.asn, L"AS13335", "parse ipapi asn field");

    IpInfo bad;
    ok = ParseIpInfo(R"({"error":true})", L"test", bad, err);
    Check(!ok, "parse missing ip fails");
    Check(!err.empty(), "parse missing ip has error text");
}

void TestShortAsn()
{
    IpInfo a;
    a.asn = L"AS20473";
    a.org = L"The Constant Company, LLC";
    CheckW(ShortAsn(a), L"AS20473 Vultr", "short asn vultr");

    IpInfo b;
    b.asn = L"AS14061";
    b.org = L"DigitalOcean, LLC";
    CheckW(ShortAsn(b), L"AS14061 DO", "short asn digitalocean");

    IpInfo c;
    CheckW(ShortAsn(c), L"", "short asn empty");
}

void TestAgo()
{
    uint64_t now = 1000000000000ULL;   // 足够大，减一天也不会下溢
    CheckW(Ago(0, now), L"未更新", "ago zero");
    CheckW(Ago(now, now), L"刚刚", "ago same");
    CheckW(Ago(now - 130ULL * 10000000ULL, now), L"2分钟前", "ago 2 minutes");
    CheckW(Ago(now - 59ULL * 10000000ULL, now), L"59秒前", "ago 59 seconds");
    CheckW(Ago(now - 24ULL * 3600ULL * 10000000ULL, now), L"1天前", "ago 1 day");

    uint64_t out = 0;
    Check(!FileTimeMinusSeconds(10, 1, out), "filetime underflow");
}

void TestUrls()
{
    CheckW(SourceHost(L"https://ipinfo.io:443/json"), L"ipinfo.io", "source host :443");
    CheckW(SourceHost(L"http://example.com:80/a"), L"example.com", "source host :80");
    CheckW(SourceHost(L"https://ipapi.co/json/"), L"ipapi.co", "source host plain");
}

} // namespace

int main()
{
    TestJson();
    TestParse();
    TestShortAsn();
    TestAgo();
    TestUrls();

    if (g_failures == 0) {
        printf("\nall tests passed\n");
        return 0;
    }
    printf("\n%d test(s) failed\n", g_failures);
    return 1;
}
