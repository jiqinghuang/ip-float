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
    in.v6.ip = L"45.77.144.7";       // 与 v4 同源
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

// 坑 2：双栈出口不同源
void TestExitMismatch()
{
    printf("\n-- exit mismatch --\n");
    ProbeInput in = Base();
    in.v6.ip = L"2408:84e2:480:77c9::1";
    Diagnosis d = Diagnose(in);
    CheckSev(d.Find(LineId::ExitV6)->sev, Sev::Bad, "mismatch: exit v6 bad");
    CheckSev(d.verdict, Sev::Bad, "mismatch: verdict bad");
    Check(d.evidence.find(L"2408") != std::wstring::npos, "mismatch: evidence names v6");
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
    TestExitMismatch();
    TestTimezone();
    TestProxyOverlay();
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
