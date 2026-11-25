#include <windows.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <string>

#include "app.h"
#include "detail_dlg.h"
#include "diag.h"
#include "selftest.h"
#include "util.h"

using namespace Gdiplus;

namespace {

void SetDpiAwareness()
{
    // manifest 已声明 PerMonitorV2；这里再设一次是给没有 manifest 的构建兜底
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI* Fn)(void*);
        Fn fn = (Fn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn && fn((void*)-4)) return;   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    }
    SetProcessDPIAware();
}

bool TakeArg(int argc, LPWSTR* argv, int& i, std::wstring& out)
{
    if (i + 1 >= argc) return false;
    out = argv[++i];
    return true;
}

bool ParseArgs(AppOptions& opt, std::wstring& renderPath, float& renderScale,
               std::wstring& diagPath, std::wstring& renderDlgPath)
{
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return true;

    bool ok = true;
    for (int i = 1; i < argc && ok; ++i) {
        std::wstring a = argv[i];
        if (a == L"--demo") {
            opt.demo = true;
        } else if (a == L"--shot") {
            ok = TakeArg(argc, argv, i, opt.shotPath);
        } else if (a == L"--shot-dlg") {
            ok = TakeArg(argc, argv, i, opt.dlgShotPath);
        } else if (a == L"--render-dlg") {
            ok = TakeArg(argc, argv, i, renderDlgPath);
        } else if (a == L"--diag") {
            ok = TakeArg(argc, argv, i, diagPath);
        } else if (a == L"--redact") {
            opt.diagRedact = true;
        } else if (a == L"--fail-on") {
            std::wstring v;
            ok = TakeArg(argc, argv, i, v);
            if (ok) {
                if (v == L"bad")       opt.diagFailOn = 1;
                else if (v == L"warn") opt.diagFailOn = 2;
                else ok = false;   // 只接受 bad / warn，拼错就报错，别静默忽略
            }
        } else if (a == L"--render") {
            ok = TakeArg(argc, argv, i, renderPath);
        } else if (a == L"--render-scale") {
            std::wstring v;
            if (TakeArg(argc, argv, i, v)) {
                float f = (float)_wtof(v.c_str());
                if (f >= 0.5f && f <= 4.0f) renderScale = f;
            }
        } else if (a == L"--source") {
            ok = TakeArg(argc, argv, i, opt.sourceOverride);
            if (ok) opt.sourceOverridden = true;
        } else if (a == L"--ini") {
            ok = TakeArg(argc, argv, i, opt.iniPath);
        } else if (a == L"--interval") {
            std::wstring v;
            if (TakeArg(argc, argv, i, v)) opt.intervalOverride = _wtoi(v.c_str());
        }
        // 未知参数忽略，保持和旧版一致
    }
    LocalFree(argv);

    if (!ok) return false;
    if (!renderPath.empty() && (!opt.shotPath.empty() || !opt.dlgShotPath.empty()))
        return false;   // --render 不和屏幕截图混用
    if (!opt.shotPath.empty() && !opt.dlgShotPath.empty())
        return false;   // 两个自检截图一次只能拍一个
    if (!diagPath.empty() &&
        (!renderPath.empty() || !opt.shotPath.empty() || !opt.dlgShotPath.empty()))
        return false;   // --diag 是独立模式，不和截图/渲染混用
    if (!renderDlgPath.empty() &&
        (!renderPath.empty() || !diagPath.empty() || !opt.shotPath.empty() || !opt.dlgShotPath.empty()))
        return false;   // --render-dlg 也是独立模式

    // 设置窗口抓图永远用演示数据；屏幕卡片抓图默认也走演示，
    // 只有显式给了 --source 才去取真实数据。
    if (!opt.dlgShotPath.empty()) {
        opt.demo = true;
    } else if (!opt.shotPath.empty() && opt.sourceOverride.empty()) {
        opt.demo = true;
    }

    return true;
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
{
    SetDpiAwareness();

    AppOptions opt;
    std::wstring renderPath;
    std::wstring diagPath;
    std::wstring renderDlgPath;
    float renderScale = 2.0f;
    if (!ParseArgs(opt, renderPath, renderScale, diagPath, renderDlgPath)) return 2;

    GdiplusStartupInput gdiIn;
    ULONG_PTR gdiToken = 0;
    if (GdiplusStartup(&gdiToken, &gdiIn, nullptr) != Ok) return 3;

    int rc = 0;
    if (!renderPath.empty()) {
        std::wstring err;
        rc = selftest::RenderDemoCard(renderPath, renderScale, err) ? 0 : 1;
    } else if (!renderDlgPath.empty()) {
        std::wstring err;
        const uint64_t now = NowFileTime();
        const Diagnosis d = selftest::MakeDemoDiagnosis(now);
        rc = RenderDiagToPng(renderDlgPath, renderScale, d, now, err) ? 0 : 1;
    } else if (!diagPath.empty()) {
        std::wstring err;
        int code = 0;
        DiagFailOn failOn = opt.diagFailOn == 1 ? DiagFailOn::Bad
                          : opt.diagFailOn == 2 ? DiagFailOn::Warn
                                                : DiagFailOn::None;
        rc = RunDiag(diagPath, opt.diagRedact, failOn, err, code) ? code : 1;
    } else {
        rc = RunApp(opt);
    }

    GdiplusShutdown(gdiToken);
    return rc;
}
