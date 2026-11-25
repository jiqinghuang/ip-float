// 纯逻辑单测：JSON 解析 / IpInfo / ShortAsn / Ago / URL host。
// 不建窗口、不联网；build.ps1 -Test 会编译并运行它。
#include "../src/http.h"
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

    const std::string who = R"({"ip":"203.0.113.42",)"
                            R"("connection":{"asn":17621,"org":"China Unicom"},)"
                            R"("timezone":{"offset":28800,"utc":"+08:00"}})";
    CheckStr(JsonStr(who, "connection.org"), "China Unicom", "json nested string");
    bool found = false;
    long long asn = JsonInt(who, "connection.asn", found);
    Check(found && asn == 17621, "json nested int");
    found = false;
    long long off = JsonInt(who, "timezone.offset", found);
    Check(found && off == 28800, "json nested offset");
    found = false;
    Check(JsonBool(R"({"ok":true})", "ok", found) && found, "json bool true");
    found = false;
    Check(!JsonBool(R"({"ok":false})", "ok", found) && found, "json bool false");
    found = false;
    JsonInt(R"({"asn":"17621"})", "asn", found);
    Check(found, "json int accepts quoted number");
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

    IpInfo who;
    ok = ParseIpInfo(
        R"({"ip":"2001:db8::42","country_code":"JP","region":"Tokyo",)"
        R"("city":"Chiyoda","connection":{"asn":20473,"org":"Vultr"},)"
        R"("timezone":{"offset":32400,"utc":"+09:00"}})",
        L"https://ipwho.is/", who, err);
    Check(ok, "parse ipwho ok");
    CheckW(who.ip, L"2001:db8::42", "parse ipwho v6 ip");
    CheckW(who.asn, L"AS20473", "parse ipwho nested asn");
    CheckW(who.org, L"Vultr", "parse ipwho nested org");
    Check(who.utcOffsetMinutes == 540, "parse ipwho timezone offset");

    IpInfo bad;
    ok = ParseIpInfo(R"({"ip":"not-an-ip"})", L"test", bad, err);
    Check(!ok, "parse invalid ip fails");
    ok = ParseIpInfo(R"({"error":true})", L"test", bad, err);
    ok = ParseIpInfo(R"({"error":true})", L"test", bad, err);
    Check(!ok, "parse missing ip fails");
    Check(!err.empty(), "parse missing ip has error text");
}

void TestUtcOffset()
{
    Check(ParseUtcOffset("+0800") == 480, "utc +0800");
    Check(ParseUtcOffset("-0500") == -300, "utc -0500");
    Check(ParseUtcOffset("+08:00") == 480, "utc +08:00");
    Check(ParseUtcOffset("-05:30") == -330, "utc -05:30");
    Check(ParseUtcOffset("08:00") == kNoOffset, "utc missing sign");
    Check(ParseUtcOffset("+8:00") == kNoOffset, "utc short hour");
    Check(ParseUtcOffset("+25:00") == kNoOffset, "utc hour range");
    Check(ParseUtcOffset("") == kNoOffset, "utc empty");
}

void TestExpectedText()
{
    std::wstring c, a;
    ParseExpectedText(L"JP / AS20473", c, a);
    CheckW(c, L"JP", "expected country");
    CheckW(a, L"AS20473", "expected asn");

    ParseExpectedText(L"jp,20473", c, a);
    CheckW(c, L"JP", "expected country lower");
    CheckW(a, L"AS20473", "expected asn numeric");

    ParseExpectedText(L"20473", c, a);
    CheckW(c, L"", "expected only asn: no country");
    CheckW(a, L"AS20473", "expected only asn");

    ParseExpectedText(L"as20473 / jp", c, a);
    CheckW(c, L"JP", "expected reversed country");
    CheckW(a, L"AS20473", "expected reversed asn");

    ParseExpectedText(L"  ", c, a);
    CheckW(c, L"", "expected blank country");
    CheckW(a, L"", "expected blank asn");
}

void TestTlsErrors()
{
    Check(IsTlsCertErrorText(L"12175：TLS 安全通道失败"), "tls cert 12175");
    Check(IsTlsCertErrorText(L"12038：证书域名不匹配"), "tls cert 12038");
    Check(!IsTlsCertErrorText(L"12029：无法连接"), "tls non-cert 12029");
    Check(!IsTlsCertErrorText(L"HTTP 403"), "tls non-cert http");
    Check(!IsTlsCertErrorText(L""), "tls empty");
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
    CheckW(SourceHost(L"https://user:token@example.com/json"), L"example.com",
           "source host strips userinfo");
}

void TestIpKind()
{
    Check(ClassifyIp(L"203.0.113.42") == IpKind::V4, "ip kind v4");
    Check(ClassifyIp(L" 203.0.113.42 \t") == IpKind::V4, "ip kind v4 trims spaces");
    Check(ClassifyIp(L"2001:db8::42") == IpKind::V6, "ip kind v6");
    Check(ClassifyIp(L"::1") == IpKind::V6, "ip kind v6 loopback");
    Check(ClassifyIp(L"999.1.1.1") == IpKind::None, "ip kind rejects bad v4");
    Check(ClassifyIp(L"2001:db8::zz") == IpKind::None, "ip kind rejects bad v6");
    Check(ClassifyIp(L"") == IpKind::None, "ip kind empty");
    Check(ClassifyIp(L"ipinfo.io") == IpKind::None, "ip kind rejects host");
}

} // namespace

int main()
{
    TestJson();
    TestParse();
    TestUtcOffset();
    TestTlsErrors();
    TestExpectedText();
    TestShortAsn();
    TestAgo();
    TestUrls();
    TestIpKind();

    if (g_failures == 0) {
        printf("\nall tests passed\n");
        return 0;
    }
    printf("\n%d test(s) failed\n", g_failures);
    return 1;
}
