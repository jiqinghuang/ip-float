#pragma once

#include <string>

struct AppOptions {
    bool         demo = false;
    std::wstring iniPath;          // --ini
    std::wstring sourceOverride;   // --source
    bool         sourceOverridden = false;
    int          intervalOverride = 0;   // --interval
    std::wstring shotPath;         // --shot（屏幕抓图 smoke test）
    std::wstring dlgShotPath;      // --shot-dlg
    bool         diagRedact = false;   // --diag --redact
    int          diagFailOn = 0;       // 0 = 不因判定失败；1 = BAD；2 = BAD/WARN
};

// 要求 GDI+ 已由调用方启动。返回进程退出码。
int RunApp(const AppOptions& opt);
