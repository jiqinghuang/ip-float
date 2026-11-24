// 纯逻辑单测：诊断判定 + 徽章。不建窗口、不联网。
// 重点覆盖两个坑：
//   1) TUN 只接管 v4、本机却有公网 v6 —— 必须判成 BAD，不能判 OK
//   2) 探测失败 —— 不能冒充「未发现泄漏」
#include "../src/netinfo.h"
#include "../src/probe.h"
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

void CheckSev(Sev got, Sev want, const char* what)
{
    if (got == want) {
        printf("ok   %s\n", what);
    } else {
        ++g_failures;
        printf("FAIL %s\n  got  = [%s]\n  want = [%s]\n", what, SevTag(got), SevTag(want));
    }
}

void CheckW(const std::wstring& got, const std::wstring& want, const char* what)
{
    if (got == want) {
        printf("ok   %s\n", what);
    } else {
        ++g_failures;
        printf("FAIL %s\n  got  = [%s]\n  want = [%s]\n",
               what, WideToUtf8(got).c_str(), WideToUtf8(want).c_str());
    }
}

// 干净场景：TUN 接管 v4+v6，双栈出口同源，时区一致，系统代理已关
ProbeInput Base()
{
    ProbeInput in;
    in.facts.lanIp = L"172.20.10.12";
    in.facts.lanAdapter = L"WLAN";
    in.facts.localOffsetMinutes = 480;
    in.facts.localTzName = L"China Standard Time";
    in.facts.tunDefaultV4 = true;
    in.facts.tunDefaultV6 = true;
    in.facts.tunIfName = L"singbox_tun";
    in.facts.hasPublicV6 = true;
    in.facts.publicV6 = L"2408:84e2:480:77c9::1";
    in.facts.proxyConfigured = true;
    in.facts.proxyEnabled = false;
    in.facts.proxyServer = L"127.0.0.1:10808";

    in.v4.ok = true;
    in.v4.ip = L"45.77.144.7";
    in.v4.country = L"United States";
    in.v4.region = L"New Jersey";
    in.v4.asn = L"AS20473";
    in.v4.org = L"Vultr";
    in.v4.isHosting = true;
    in.v4.utcOffsetMinutes = 480;    // 与本机一致，先测「全绿」

    in.v6.ok = true;
    in.v6.ip = L"2001:db8::42";      // 真正的 v6 地址（文档前缀）
    in.v6.asn = L"AS20473";          // 与 v4 同 ASN = 同源
    return in;
}

void TestClean()
{
    printf("\n-- clean --\n");
    Diagnosis d = Diagnose(Base());
    CheckSev(d.verdict, Sev::Ok, "clean: verdict ok");
    CheckW(d.summary, L"未发现泄漏", "clean: summary");
    Check(d.lines.size() == 8, "clean: 8 lines");
    CheckSev(d.Find(LineId::Tunnel)->sev, Sev::Ok, "clean: tunnel ok");
    Check(!d.Find(LineId::Tunnel)->na, "clean: tunnel is applicable when TUN is on");
    CheckSev(d.Find(LineId::V6Route)->sev, Sev::Ok, "clean: v6 route ok");
    CheckSev(d.Find(LineId::ExitV6)->sev, Sev::Ok, "clean: exit v6 same as v4");
    CheckSev(d.Find(LineId::Proxy)->sev, Sev::Ok, "clean: path is TUN");
    CheckW(d.Find(LineId::Proxy)->value, L"TUN", "clean: path value");
}

// 坑 1：v6 没被接管，但本机有公网 v6 —— 结构性泄漏
void TestV6NotTunneled()
{
    printf("\n-- v6 not tunneled --\n");
    ProbeInput in = Base();
    in.facts.tunDefaultV6 = false;
    in.v6.ok = false;                 // 直连 v6 大概率打不通
    Diagnosis d = Diagnose(in);
    CheckSev(d.Find(LineId::V6Route)->sev, Sev::Bad, "v6leak: v6 route bad");
    CheckSev(d.Find(LineId::Tunnel)->sev, Sev::Warn, "v6leak: tunnel warns about v6");
    CheckSev(d.verdict, Sev::Bad, "v6leak: verdict bad");
    Check(d.summary.find(L"泄漏") != std::wstring::npos, "v6leak: summary mentions leak");
}

// 坑 1 的反面：没有公网 v6 时不能误报
void TestNoPublicV6()
{
    printf("\n-- no public v6 --\n");
    ProbeInput in = Base();
    in.facts.hasPublicV6 = false;
    in.facts.publicV6.clear();
    in.facts.tunDefaultV6 = false;
    in.v6.ok = false;
    Diagnosis d = Diagnose(in);
    CheckSev(d.Find(LineId::V6Route)->sev, Sev::Skip, "nov6: v6 route skipped");
    CheckSev(d.Find(LineId::ExitV6)->sev, Sev::Skip, "nov6: exit v6 skipped");
    CheckSev(d.verdict, Sev::Ok, "nov6: verdict still ok");
}

// 远端回显拿不到时：出口地址必须写「未取到」，本机公网 IPv6 单独一行，不能混在一起。
void TestV6LocalFallback()
{
    printf("\n-- v6 local fallback --\n");
    ProbeInput in = Base();
    in.v6.ok = true;
    in.v6.ip.clear();
    Diagnosis d = Diagnose(in);

    const ProbeLine* exit = d.Find(LineId::ExitV6);
    Check(exit != nullptr, "v6fallback: exit line exists");
    if (exit) {
        CheckW(exit->value, L"未取到", "v6fallback: exit value is not the local address");
        CheckSev(exit->sev, Sev::Skip, "v6fallback: unknown exit is skip");
    }

    const ProbeLine* local = d.Find(LineId::V6Local);
    Check(local != nullptr, "v6fallback: local line exists");
    if (local) {
        CheckW(local->value, in.facts.publicV6, "v6fallback: local line shows public v6");
        CheckSev(local->sev, Sev::Skip, "v6fallback: local line is informational");
    }
}

// 坑 2：双栈出口不同源
void TestExitMismatch()
{
    printf("\n-- exit mismatch --\n");
    ProbeInput in = Base();
    in.v6.ip = L"2408:84e2:480:77c9::1";
    in.v6.asn = L"AS4134";       // 和 v4 的 AS20473 不同
    Diagnosis d = Diagnose(in);
    CheckSev(d.Find(LineId::ExitV6)->sev, Sev::Warn, "mismatch: exit v6 warns");
    CheckSev(d.verdict, Sev::Warn, "mismatch: verdict warn");
    Check(d.Find(LineId::ExitV6)->note.find(L"AS4134") != std::wstring::npos,
          "mismatch: note names the v6 asn");
}

// 正常双栈：地址不同但 ASN 相同 —— 不能报警
void TestExitSameAsn()
{
    puts("== same asn, different address ==");
    ProbeInput in = Base();
    in.v6.ip = L"2600:1f18::1";
    in.v6.asn = L"AS20473";
    Diagnosis d = Diagnose(in);
    CheckSev(d.Find(LineId::ExitV6)->sev, Sev::Ok, "same-asn: ok despite different address");
    Check(d.Find(LineId::ExitV6)->value.find(L"同源") != std::wstring::npos,
          "same-asn: value says tongyuan");
}

// 拿不到 v6 归属：只报地址，不判也不警
void TestExitNoAsn()
{
    puts("== v6 without asn ==");
    ProbeInput in = Base();
    in.v6.ip = L"2600:1f18::1";
    in.v6.asn.clear();
    Diagnosis d = Diagnose(in);
    CheckSev(d.Find(LineId::ExitV6)->sev, Sev::Skip, "no-asn: skip, not warn");
    CheckSev(d.verdict, Sev::Ok, "no-asn: verdict stays ok");
}

void TestTimezone()
{
    printf("\n-- timezone --\n");
    ProbeInput in = Base();
    in.v4.utcOffsetMinutes = -300;    // 美国东部冬令时
    Diagnosis d = Diagnose(in);
    CheckSev(d.Find(LineId::Tz)->sev, Sev::Warn, "tz: mismatch warns");
    CheckSev(d.verdict, Sev::Warn, "tz: verdict warn");
    Check(d.Find(LineId::Tz)->note.find(L"-5") != std::wstring::npos, "tz: note shows ip offset");

    ProbeInput unknown = Base();
    unknown.v4.utcOffsetMinutes = kUnknownOffset;
    CheckSev(Diagnose(unknown).Find(LineId::Tz)->sev, Sev::Skip, "tz: unknown is skip");
}

void TestProxyOverlay()
{
    printf("\n-- proxy overlay --\n");
    ProbeInput in = Base();
    in.facts.proxyEnabled = true;     // TUN 开着还留着系统代理
    Diagnosis d = Diagnose(in);
    CheckSev(d.Find(LineId::Proxy)->sev, Sev::Warn, "proxy: overlay warns");
    CheckSev(d.verdict, Sev::Warn, "proxy: verdict warn");

    ProbeInput pac = Base();
    pac.facts.pacConfigured = true;
    CheckSev(Diagnose(pac).Find(LineId::Proxy)->sev, Sev::Warn, "proxy: pac warns");
}

// 期望出口：配置后只影响总判定和诊断行，不新增卡片徽章；空配置不产生该行。
void TestExpectation()
{
    printf("\n-- expected exit --\n");
    ProbeInput in = Base();
    in.v4.country = L"JP";
    in.expectedCountry = L"JP";
    in.expectedAsn = L"20473";       // 应规范化为 AS20473
    Diagnosis d = Diagnose(in);
    const ProbeLine* ok = d.Find(LineId::Expect);
    Check(ok != nullptr, "expect: line exists when configured");
    if (ok) CheckSev(ok->sev, Sev::Ok, "expect: match ok");
    CheckSev(d.verdict, Sev::Ok, "expect: matching verdict ok");

    ProbeInput bad = Base();
    bad.v4.country = L"US";
    bad.expectedCountry = L"JP";
    bad.expectedAsn = L"AS20473";
    Diagnosis db = Diagnose(bad);
    const ProbeLine* mismatch = db.Find(LineId::Expect);
    Check(mismatch != nullptr, "expect: mismatch line exists");
    if (mismatch) CheckSev(mismatch->sev, Sev::Warn, "expect: mismatch warns");
    CheckSev(db.verdict, Sev::Warn, "expect: mismatch verdict warn");

    ProbeInput none = Base();
    Check(none.expectedCountry.empty() && none.expectedAsn.empty(), "expect: default off");
    Check(Diagnose(none).Find(LineId::Expect) == nullptr, "expect: no line when unset");
}

// 坑 3：探测失败不能冒充「未发现泄漏」
void TestProbeFailed()
{
    printf("\n-- probe failed --\n");
    ProbeInput in = Base();
    in.v4.ok = false;
    in.v4.ip.clear();
    in.v4.err = L"WinHttpSendRequest 失败（12002）";
    in.v6.ok = false;
    Diagnosis d = Diagnose(in);
    CheckSev(d.Find(LineId::ExitV4)->sev, Sev::Bad, "failed: exit v4 bad");
    CheckW(d.summary, L"出口探测失败，无法判断", "failed: summary does not claim clean");
}

// 没开隧道/代理时，v6 直连是正常上网，不该报黄
void TestNoProxyV6Direct()
{
    printf("\n-- no tunnel, v6 direct --\n");
    ProbeInput in = Base();
    in.facts.tunDefaultV4 = false;
    in.facts.tunDefaultV6 = false;
    in.facts.proxyEnabled = false;
    in.facts.proxyConfigured = false;
    in.facts.proxyServer.clear();
    Diagnosis d = Diagnose(in);
    CheckSev(d.Find(LineId::Tunnel)->sev, Sev::Skip, "novpn: tunnel skipped");
    CheckSev(d.Find(LineId::V6Route)->sev, Sev::Skip, "novpn: v6 direct is not a leak");
    CheckSev(d.verdict, Sev::Ok, "novpn: verdict ok");
    // 「不适用」必须和「没测到」区分开，否则健康状态看起来像探测失败
    Check(d.Find(LineId::Tunnel)->na, "novpn: tunnel marked not-applicable");
    Check(d.Find(LineId::V6Route)->na, "novpn: v6 route marked not-applicable");
    std::vector<Badge> b = BuildBadges(d);
    Check(b[0].na, "novpn: TUN badge is hollow");
    Check(b[1].na, "novpn: V6 badge is hollow");
    Check(b[2].na, "novpn: 路径 badge is hollow when nothing is configured");
}

void TestBadges()
{
    printf("\n-- badges --\n");
    Diagnosis d = Diagnose(Base());
    std::vector<Badge> b = BuildBadges(d);
    Check(b.size() == 5, "badges: 5 badges");
    CheckW(b[0].label, L"TUN", "badges: 0 is TUN");
    CheckW(b[1].label, L"V6", "badges: 1 is V6");
    CheckW(b[2].label, L"路径", "badges: 2 is 路径");
    CheckW(b[3].label, L"时区", "badges: 3 is 时区");
    CheckW(b[4].label, L"DNS", "badges: 4 is DNS");
    CheckSev(b[4].sev, Sev::Skip, "badges: DNS is skip before it is checked");

    ProbeInput in = Base();
    in.facts.tunDefaultV6 = false;
    in.v6.ok = false;
    std::vector<Badge> b2 = BuildBadges(Diagnose(in));
    CheckSev(b2[1].sev, Sev::Bad, "badges: V6 badge reflects leak");
}

} // namespace

int main()
{
    TestClean();
    TestV6NotTunneled();
    TestNoPublicV6();
    TestV6LocalFallback();
    TestExitMismatch();
    TestExitSameAsn();
    TestExitNoAsn();
    TestTimezone();
    TestProxyOverlay();
    TestExpectation();
    TestNoProxyV6Direct();
    TestProbeFailed();
    TestBadges();

    if (g_failures == 0) {
        printf("\nall tests passed\n");
        return 0;
    }
    printf("\n%d test(s) failed\n", g_failures);
    return 1;
}
