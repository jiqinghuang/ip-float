// 配置读写单测：SaveConfig → LoadConfig 往返，重点覆盖新增的期望出口/时区开关。
#include "../src/config.h"
#include "../src/util.h"

#include <windows.h>
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

void CheckW(const std::wstring& got, const std::wstring& want, const char* what)
{
    if (got == want) {
        printf("ok   %s\n", what);
    } else {
        ++g_failures;
        printf("FAIL %s\n", what);
    }
}

// 放在测试 exe 旁边（build.ps1 的 OutDir），避免受 %TEMP% 的权限/重定向策略影响。
std::wstring TestIni()
{
    std::wstring p = ExePath();
    const size_t pos = p.find_last_of(L"\\/");
    if (pos != std::wstring::npos) p.resize(pos + 1);
    return p + L"ip-float-test-config.ini";
}

} // namespace

int main()
{
    Check(!ExePath().empty(), "config: exe path");
    const std::wstring path = TestIni();
    DeleteFileW(path.c_str());

    Config c;
    c.intervalMinutes = 7;
    c.opacityPercent = 88;
    c.topMost = false;
    c.autoStart = true;
    c.x = 12;
    c.y = -34;
    c.panelOpen = true;
    c.source = L"https://example.com/ip.json";
    c.expectedCountry = L"JP";
    c.expectedAsn = L"AS20473";
    c.ignoreTimezone = true;

    const bool saved = SaveConfig(path, c);
    if (!saved) printf("save failed: path=[%ls] err=%lu\n", path.c_str(), GetLastError());
    Check(saved, "config: save");

    Config d;
    LoadConfig(path, d);
    Check(d.intervalMinutes == 7, "config: interval");
    Check(d.opacityPercent == 88, "config: opacity");
    Check(!d.topMost, "config: topMost");
    Check(d.autoStart, "config: autoStart");
    Check(d.x == 12 && d.y == -34, "config: position");
    Check(d.panelOpen, "config: panelOpen");
    CheckW(d.source, L"https://example.com/ip.json", "config: source");
    CheckW(d.expectedCountry, L"JP", "config: expected country");
    CheckW(d.expectedAsn, L"AS20473", "config: expected asn");
    Check(d.ignoreTimezone, "config: ignore timezone");

    // 清空期望出口：空值应该删掉键，读回来是空
    Config e = d;
    e.expectedCountry.clear();
    e.expectedAsn.clear();
    e.ignoreTimezone = false;
    Check(SaveConfig(path, e), "config: save empty expectation");
    Config f;
    LoadConfig(path, f);
    Check(f.expectedCountry.empty(), "config: empty country");
    Check(f.expectedAsn.empty(), "config: empty asn");
    Check(!f.ignoreTimezone, "config: ignore timezone off");

    DeleteFileW(path.c_str());
    if (g_failures == 0) {
        printf("\nall tests passed\n");
        return 0;
    }
    printf("\n%d test(s) failed\n", g_failures);
    return 1;
}
