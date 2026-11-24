#include "app.h"

#include "card.h"
#include "config.h"
#include "detail_dlg.h"
#include "diag.h"
#include "http.h"
#include "ipinfo.h"
#include "selftest.h"
#include "settings_dlg.h"
#include "tray.h"
#include "util.h"

#include <windows.h>
#include <process.h>
#include <shellapi.h>

#include <atomic>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

using namespace Gdiplus;

namespace {

const wchar_t* kWndClass = L"ip-float-window";
const wchar_t* kMutexName = L"Local\\ip-float-single-instance";

const UINT ID_TIMER_REFRESH    = 1;
const UINT ID_TIMER_TICK       = 2;
const UINT ID_TIMER_SHOT       = 3;
const UINT ID_TIMER_FLASH      = 5;
const UINT ID_TIMER_TRAY_RETRY = 6;

const UINT WM_APP_FETCHED  = WM_APP + 1;   // 第一阶段：IP / 地区回来了
const UINT WM_APP_TRAY     = WM_APP + 2;
const UINT WM_APP_ACTIVATE = WM_APP + 3;   // 第二个实例让第一个把卡片亮出来
const UINT WM_APP_DIAGNOSED = WM_APP + 4;  // 第二阶段：出口体检 / 徽章回来了

enum {
    IDM_REFRESH = 100,
    IDM_INTERVAL_BASE = 200,
    IDM_SETTINGS = 300,
    IDM_DIAG,
    IDM_COPY,
    IDM_TOPMOST,
    IDM_AUTOSTART,
    IDM_TOGGLE,
    IDM_QUIT,
};

const int kIntervals[] = { 1, 3, 5, 10, 30, 60 };
const int kIntervalCount = 6;

const UINT kTrayRetryMs = 5000;
const int  kTrayRetries = 3;

struct FetchJob {
    HWND hwnd = nullptr;
    std::vector<std::wstring> sources;
    std::wstring expectedCountry;
    std::wstring expectedAsn;
};

// 第一阶段：只带 IP，先让卡片显示出来。
struct FetchResult {
    bool         ok = false;
    IpInfo       info;
    std::wstring err;
    std::wstring errFull;
};

// 第二阶段：体检结果，到了再更新徽章和诊断窗口。
struct DiagResult {
    std::vector<Badge> badges;          // 卡片第三行
    Sev                verdict = Sev::Skip;
    Diagnosis          diag;            // 诊断窗口用的完整报告
};

struct App {
    AppOptions opt;
    HINSTANCE  inst = nullptr;
    HWND       hwnd = nullptr;
    HICON      icon = nullptr;
    bool       iconOwned = false;
    HANDLE     mutex = nullptr;

    IniLocation ini;
    Config      cfg;
    bool        persistConfig = true;
    bool        configSaveFailed = false;

    std::vector<std::wstring> sources;

    IpInfo       info;
    bool         hasInfo = false;
    bool         lastError = false;
    uint64_t     updatedAt = 0;
    std::wstring lastErrMsg;
    std::wstring lastErrFull;
    std::vector<Badge> badges;
    Sev          verdict = Sev::Skip;
    Diagnosis    diag;
    uint64_t     diagAt = 0;
    bool         pendingPanelOpen = false;   // 等第一轮数据到了再开面板，避免空窗口闪跳

    // 只有 UI 线程读写 busy；worker 不持有任何 App 指针，避免退出时 use-after-free
    std::atomic<bool> busy{ false };
    bool              fetchPending = false;
    bool              timerWarn = false;

    float scale = 1.0f;
    int   flashTicks = 0;
    bool  shotDone = false;

    Tray tray;
    int  trayRetries = 0;
};

App* g_app = nullptr;

bool FetchNow(const std::vector<std::wstring>& sources, IpInfo& out,
              std::wstring& err, std::wstring& errFull)
{
    std::wstring lastErr = L"没有可用数据源";
    std::wstring lastFull = lastErr;
    for (const std::wstring& url : sources) {
        std::string body;
        std::wstring e;
        if (HttpGet(url, body, e)) {
            if (ParseIpInfo(body, url, out, e)) return true;
        }
        lastErr = e;
        lastFull = url + L" → " + e;
    }
    err = lastErr;
    errFull = lastFull;
    return false;
}

unsigned __stdcall FetchThread(void* param)
{
    std::unique_ptr<FetchJob> job((FetchJob*)param);
    auto* result = new FetchResult();
    result->ok = FetchNow(job->sources, result->info, result->err, result->errFull);

    // 先发 IP：卡片不用等后面的 v6/DNS 探测。这里必须拷贝一份 info，
    // 因为消息一旦投递，接收方会在 UI 线程 delete result。
    const IpInfo base = result->info;
    if (!PostMessageW(job->hwnd, WM_APP_FETCHED, 0, (LPARAM)result)) {
        // 窗口已经销毁（进程正在退出）：结果没人要了。
        // 不要回写任何 UI 状态——App 可能已经析构。
        delete result;
        return 0;
    }

    // 出口探测只依赖「拿到的 IP」，所以抓取失败也照样能算（v4 会标成探测失败）
    ProbeInput in;
    ProbeExits(base, in, job->expectedCountry, job->expectedAsn);
    Diagnosis diag = Diagnose(in);
    auto* diagResult = new DiagResult();
    diagResult->badges = BuildBadges(diag);
    diagResult->verdict = diag.verdict;
    diagResult->diag = diag;

    if (!PostMessageW(job->hwnd, WM_APP_DIAGNOSED, 0, (LPARAM)diagResult)) {
        delete diagResult;
    }
    return 0;
}

void StartFetch(App& app)
{
    if (app.opt.demo) return;

    if (app.busy.exchange(true)) {
        // 正在抓取就记账，抓完再补一次；否则“改间隔立即刷新”会被静默吃掉
        app.fetchPending = true;
        return;
    }

    auto* job = new FetchJob();
    job->hwnd = app.hwnd;
    job->sources = app.sources;
    job->expectedCountry = app.cfg.expectedCountry;
    job->expectedAsn = app.cfg.expectedAsn;

    HANDLE t = (HANDLE)_beginthreadex(nullptr, 0, FetchThread, job, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {
        delete job;
        app.busy.store(false);
    }
}

// 诊断窗口只负责显示；数据来自与卡片同一轮的抓取，所以不需要额外请求
void PersistConfig(App& app);   // 定义在后面

void OpenDiagWindow(App& app)
{
    ShowDiagWindow(app.hwnd, app.diag, app.diagAt);
    if (!app.cfg.panelOpen) {
        app.cfg.panelOpen = true;
        PersistConfig(app);
    }
    // 窗口先显示上次结果，同时后台跑一轮新的
    SetDiagBusy(true);
    StartFetch(app);
}

void PersistConfig(App& app)
{
    if (!app.persistConfig) return;
    app.configSaveFailed = !SaveConfig(app.ini.path, app.cfg);
}

void SavePosition(App& app)
{
    if (!app.persistConfig || !IsWindow(app.hwnd)) return;
    RECT rc = {};
    if (GetWindowRect(app.hwnd, &rc)) {
        app.cfg.x = rc.left;
        app.cfg.y = rc.top;
    }
}

void ApplyAlpha(App& app)
{
    if (!app.hwnd) return;
    SetLayeredWindowAttributes(app.hwnd, 0,
                               (BYTE)(app.cfg.opacityPercent * 255 / 100), LWA_ALPHA);
}

void ApplyTopMost(App& app)
{
    if (!app.hwnd) return;
    SetWindowPos(app.hwnd, app.cfg.topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// keepPos != nullptr 时用它当目标位置（DPI 变化时系统建议的新位置）。
// 关键：有保存坐标时先按保存坐标反查显示器，否则副屏上的卡片会被主屏 rcWork 夹回来。
void ApplyLayout(App& app, const RECT* keepPos)
{
    HMONITOR mon = nullptr;
    if (keepPos) {
        mon = MonitorFromRect(keepPos, MONITOR_DEFAULTTONEAREST);
    } else if (app.cfg.x != -1 && app.cfg.y != -1) {
        POINT pt = { app.cfg.x, app.cfg.y };
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    } else {
        mon = MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST);
    }
    if (!mon) mon = MonitorFromWindow(app.hwnd, MONITOR_DEFAULTTONEAREST);

    app.scale = DpiScaleForMonitor(mon);
    CardMetrics m = ComputeCardMetrics(app.scale);

    SetWindowPos(app.hwnd, nullptr, 0, 0, m.width, m.height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    HRGN rgn = CreateRoundRectRgn(0, 0, m.width + 1, m.height + 1,
                                  m.radius * 2, m.radius * 2);
    SetWindowRgn(app.hwnd, rgn, TRUE);
    ApplyAlpha(app);

    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);

    int x = 0, y = 0;
    if (keepPos) {
        x = keepPos->left;
        y = keepPos->top;
    } else if (app.cfg.x != -1 && app.cfg.y != -1) {
        x = app.cfg.x;
        y = app.cfg.y;
    } else {
        x = mi.rcWork.right - m.width - ScalePx(24, app.scale);
        y = mi.rcWork.top + ScalePx(24, app.scale);
    }

    // DPI 变大 / 屏幕变小时可能超界，拉回来而不是让它跑出可视区
    if (x + m.width > mi.rcWork.right) x = mi.rcWork.right - m.width;
    if (y + m.height > mi.rcWork.bottom) y = mi.rcWork.bottom - m.height;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    SetWindowPos(app.hwnd, app.cfg.topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

void ResetRefreshTimer(App& app)
{
    KillTimer(app.hwnd, ID_TIMER_REFRESH);
    UINT ms = (UINT)app.cfg.intervalMinutes * 60000u;
    if (ms < 1000u) ms = 1000u;

    if (SetTimer(app.hwnd, ID_TIMER_REFRESH, ms, nullptr)) {
        app.timerWarn = false;
        return;
    }
    // 极端情况（句柄耗尽等）：至少每分钟醒一次，并在卡片上说明
    SetTimer(app.hwnd, ID_TIMER_REFRESH, 60000u, nullptr);
    app.timerWarn = true;
}

void SetInterval(App& app, int minutes)
{
    app.cfg.intervalMinutes = minutes;
    PersistConfig(app);
    ResetRefreshTimer(app);
    StartFetch(app);   // 正在抓取时会记账，抓完自动补一次
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void CopyIp(App& app)
{
    if (!app.hasInfo) return;

    for (int attempt = 0; attempt < 5; ++attempt) {
        if (!OpenClipboard(app.hwnd)) {
            Sleep(20);
            continue;
        }

        EmptyClipboard();
        size_t bytes = (app.info.ip.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem) {
            void* p = GlobalLock(mem);
            if (p) {
                memcpy(p, app.info.ip.c_str(), bytes);
                GlobalUnlock(mem);
                if (SetClipboardData(CF_UNICODETEXT, mem)) {
                    CloseClipboard();
                    return;   // 所有权已交给剪贴板
                }
            }
            GlobalFree(mem);   // SetClipboardData/GlobalLock 失败时自己释放
        }
        CloseClipboard();
        return;
    }
}

void Paint(App& app)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(app.hwnd, &ps);

    RECT rc = {};
    GetClientRect(app.hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    if (!mem || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        EndPaint(app.hwnd, &ps);
        return;
    }

    HGDIOBJ old = SelectObject(mem, bmp);
    {
        Graphics g(mem);
        CardMetrics m = ComputeCardMetrics(app.scale);
        CardText t = BuildCardText(app.info, app.hasInfo, app.lastError, app.timerWarn,
                                   app.lastErrMsg, app.updatedAt, NowFileTime());
        t.badges = app.badges;
        // 总判定覆盖圆点：有徽章时以诊断结论为准（抓取失败时保留原来的错误提示）
        if (!app.badges.empty() && !app.lastError) {
            t.dot = app.verdict == Sev::Bad  ? DotState::Bad
                  : app.verdict == Sev::Warn ? DotState::Warn
                  : app.verdict == Sev::Ok   ? DotState::Ok
                                             : DotState::Idle;
        }
        DrawCard(g, m, t, false);
    }
    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(app.hwnd, &ps);
}

void AppendStatus(App& app, HMENU menu)
{
    std::wstring status;
    if (app.hasInfo) {
        status = app.info.ip;
        if (!app.info.org.empty()) status += L" · " + app.info.org;
        std::wstring host = SourceHost(app.info.source);
        if (!host.empty()) status += L" · 源 " + host;
        status += L" · 更新于 " + LocalTimeString(app.info.at);
    } else if (app.lastError) {
        status = L"获取失败：" + (app.lastErrFull.empty() ? app.lastErrMsg : app.lastErrFull);
    } else {
        status = L"查询中…";
    }
    if (status.size() > 90) status = status.substr(0, 90) + L"…";
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
}

void BuildMenu(App& app, HMENU menu, bool fromTray)
{
    AppendStatus(app, menu);

    if (fromTray) {
        AppendMenuW(menu, MF_STRING, IDM_TOGGLE,
                    IsWindowVisible(app.hwnd) ? L"隐藏卡片" : L"显示卡片");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"立即刷新");

    HMENU sub = CreatePopupMenu();
    for (int i = 0; i < kIntervalCount; ++i) {
        UINT flags = MF_STRING;
        if (kIntervals[i] == app.cfg.intervalMinutes) flags |= MF_CHECKED;
        wchar_t label[32] = {};
        swprintf(label, 32, L"%d 分钟", kIntervals[i]);
        AppendMenuW(sub, flags, IDM_INTERVAL_BASE + (UINT)i, label);
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"刷新间隔");

    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置…");
    AppendMenuW(menu, MF_STRING, IDM_DIAG, L"网络诊断…");
    AppendMenuW(menu, MF_STRING, IDM_COPY, L"复制 IP");
    AppendMenuW(menu, MF_STRING | (app.cfg.topMost ? MF_CHECKED : 0),
                IDM_TOPMOST, L"总在最前");
    AppendMenuW(menu, MF_STRING | (app.cfg.autoStart ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"开机自启");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"退出");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    std::wstring cfgLine = L"配置：" + app.ini.path;
    if (app.ini.portable) cfgLine += L"（便携）";
    if (app.configSaveFailed) cfgLine += L"（写入失败）";
    if (cfgLine.size() > 90) cfgLine = cfgLine.substr(0, 90) + L"…";
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, cfgLine.c_str());
}

void ShowMenu(App& app, bool fromTray)
{
    POINT pt = {};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    BuildMenu(app, menu, fromTray);

    SetForegroundWindow(app.hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, app.hwnd, nullptr);
    // NOACTIVATE 窗口做菜单宿主时，补一条消息让菜单可靠消失
    PostMessageW(app.hwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void ShowSettings(App& app)
{
    SettingsDialogInput in;
    in.cfg = app.cfg;
    in.scale = app.scale;
    if (!app.opt.dlgShotPath.empty()) in.shotPath = app.opt.dlgShotPath;

    SettingsDialogResult r;
    if (!ShowSettingsDialog(app.hwnd, in, r)) return;

    bool intervalChanged = (r.interval != app.cfg.intervalMinutes);

    app.cfg.intervalMinutes = r.interval;
    app.cfg.opacityPercent = r.opacity;
    app.cfg.topMost = r.topMost;

    if (r.autoStart != app.cfg.autoStart) {
        app.cfg.autoStart = r.autoStart;
        if (!SetAutoStart(app.cfg.autoStart))
            app.cfg.autoStart = AutoStartEnabled();   // 写失败就回到注册表真实状态
    }

    PersistConfig(app);
    ApplyTopMost(app);
    ApplyAlpha(app);
    ResetRefreshTimer(app);
    if (intervalChanged) StartFetch(app);
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void AppOnIpFetched(App& app, FetchResult* r)
{
    if (r) {
        if (r->ok) {
            app.info = r->info;
            app.updatedAt = r->info.at;
            app.hasInfo = true;
            app.lastError = false;
            app.lastErrMsg.clear();
            app.lastErrFull.clear();
        } else {
            app.lastError = true;
            app.lastErrMsg = r->err;
            app.lastErrFull = r->errFull;
        }
        delete r;
    }

    // 先让第一行和第二行出来；第三行保留上一轮徽章，等 WM_APP_DIAGNOSED 再更新。
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void AppOnDiagnosed(App& app, DiagResult* r)
{
    app.busy.store(false);

    if (r) {
        app.badges = r->badges;
        app.verdict = r->verdict;
        app.diag = r->diag;
        app.diagAt = NowFileTime();
        delete r;
    }

    if (app.fetchPending) {
        app.fetchPending = false;
        StartFetch(app);
    }

    InvalidateRect(app.hwnd, nullptr, FALSE);

    if (DiagWindowOpen()) {
        UpdateDiagWindow(app.diag, app.diagAt);
    } else if (app.pendingPanelOpen) {
        // 上次退出时面板是开着的：等诊断数据齐了再一次性开正确尺寸
        app.pendingPanelOpen = false;
        ShowDiagWindow(app.hwnd, app.diag, app.diagAt);
    }

    // 截图必须等诊断也完成，否则第三行还是上一轮的徽章。
    if (!app.opt.shotPath.empty() && !app.shotDone) {
        app.shotDone = true;
        SetTimer(app.hwnd, ID_TIMER_SHOT, 700, nullptr);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    App* app = g_app;
    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);

    const UINT taskbarCreated = TrayTaskbarCreatedMessage();
    if (taskbarCreated != 0 && msg == taskbarCreated) {
        app->tray.added = false;
        if (!TrayAdd(app->tray)) {
            app->trayRetries = kTrayRetries;
            SetTimer(hwnd, ID_TIMER_TRAY_RETRY, kTrayRetryMs, nullptr);
        }
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        app->hwnd = hwnd;
        return 0;

    case WM_PAINT:
        Paint(*app);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DPICHANGED:
        // 换屏 / 改缩放比：按新 DPI 重算尺寸、圆角、透明度，位置用系统建议值
        ApplyLayout(*app, (const RECT*)lp);
        return 0;

    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE: {
        // 分辨率/任务栏变化后把卡片拉回可视区
        RECT rc = {};
        if (GetWindowRect(hwnd, &rc)) ApplyLayout(*app, &rc);
        return 0;
    }

    case WM_APP_ACTIVATE:
        // 第二个实例启动了：把已有卡片亮一下再退出。
        // 卡片是 NOACTIVATE 窗口，FlashWindow(TRUE) 自己不会停，用定长计时器驱动。
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        app->flashTicks = 8;
        SetTimer(hwnd, ID_TIMER_FLASH, 250, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == ID_TIMER_REFRESH) {
            StartFetch(*app);
        } else if (wp == ID_TIMER_TICK) {
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == ID_TIMER_SHOT) {
            KillTimer(hwnd, ID_TIMER_SHOT);
            std::wstring err;
            CaptureWindowToPng(hwnd, app->opt.shotPath, err);
            DestroyWindow(hwnd);
        } else if (wp == ID_TIMER_FLASH) {
            FlashWindow(hwnd, TRUE);
            if (--app->flashTicks <= 0 || GetForegroundWindow() == hwnd)
                KillTimer(hwnd, ID_TIMER_FLASH);
        } else if (wp == ID_TIMER_TRAY_RETRY) {
            if (TrayAdd(app->tray) || --app->trayRetries <= 0)
                KillTimer(hwnd, ID_TIMER_TRAY_RETRY);
        }
        return 0;

    case WM_DIAG_REFRESH:
        SetDiagBusy(true);
        StartFetch(*app);
        return 0;

    case WM_DIAG_CLOSED:
        app->cfg.panelOpen = false;
        PersistConfig(*app);
        return 0;

    case WM_APP_FETCHED:
        AppOnIpFetched(*app, (FetchResult*)lp);
        return 0;

    case WM_APP_DIAGNOSED:
        AppOnDiagnosed(*app, (DiagResult*)lp);
        return 0;

    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            ShowMenu(*app, true);
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
        }
        return 0;

    case WM_RBUTTONUP:
        ShowMenu(*app, false);
        return 0;

    case WM_LBUTTONDBLCLK:
        StartFetch(*app);
        return 0;

    case WM_LBUTTONDOWN:
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDM_REFRESH) {
            StartFetch(*app);
        } else if (id >= IDM_INTERVAL_BASE && id < IDM_INTERVAL_BASE + kIntervalCount) {
            SetInterval(*app, kIntervals[id - IDM_INTERVAL_BASE]);
        } else if (id == IDM_SETTINGS) {
            ShowSettings(*app);
        } else if (id == IDM_DIAG) {
            OpenDiagWindow(*app);
        } else if (id == IDM_COPY) {
            CopyIp(*app);
        } else if (id == IDM_TOPMOST) {
            app->cfg.topMost = !app->cfg.topMost;
            PersistConfig(*app);
            ApplyTopMost(*app);
        } else if (id == IDM_AUTOSTART) {
            app->cfg.autoStart = !app->cfg.autoStart;
            if (!SetAutoStart(app->cfg.autoStart))
                app->cfg.autoStart = AutoStartEnabled();
            PersistConfig(*app);
        } else if (id == IDM_TOGGLE) {
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
        } else if (id == IDM_QUIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_QUERYENDSESSION:
        SavePosition(*app);
        PersistConfig(*app);
        return TRUE;

    case WM_ENDSESSION:
        if (wp) {
            SavePosition(*app);
            PersistConfig(*app);
        }
        return 0;

    case WM_EXITSIZEMOVE:
        SavePosition(*app);
        PersistConfig(*app);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        CloseDiagWindow();
        SavePosition(*app);
        PersistConfig(*app);
        TrayRemove(app->tray);
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool RegisterMainClass(App& app)
{
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;   // 卡片双击刷新需要，否则 WM_LBUTTONDBLCLK 永远不会来
    wc.lpfnWndProc = WndProc;
    wc.hInstance = app.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.hIcon = app.icon;
    wc.hIconSm = app.icon;
    wc.lpszClassName = kWndClass;
    return RegisterClassExW(&wc) != 0;
}

} // namespace

int RunApp(const AppOptions& opt)
{
    App app;
    app.opt = opt;
    app.inst = GetModuleHandleW(nullptr);
    // 先摆一排灰徽章：首轮探测完成前卡片不空着，也避免高度变化带来的跳动
    app.badges = BuildBadges(Diagnosis());

    // 自检/截图模式只读配置：--demo、--shot、--shot-dlg 都不写回用户设置
    const bool selfTest = opt.demo || !opt.shotPath.empty() || !opt.dlgShotPath.empty();
    app.persistConfig = !selfTest;

    app.ini = ResolveIniPath(opt.iniPath);
    LoadConfig(app.ini.path, app.cfg);

    // 开机自启以注册表为准：手工删过 Run 项、或 ini 写失败时，状态才不会骗人
    app.cfg.autoStart = AutoStartEnabled();

    if (opt.intervalOverride >= 1 && opt.intervalOverride <= 1440)
        app.cfg.intervalMinutes = opt.intervalOverride;
    if (opt.sourceOverridden)
        app.cfg.source = opt.sourceOverride;

    if (!app.cfg.source.empty()) {
        app.sources.push_back(app.cfg.source);
    } else {
        app.sources.push_back(L"https://ipinfo.io/json");
        app.sources.push_back(L"https://ipapi.co/json/");
    }

    app.icon = (HICON)LoadImageW(app.inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 GetSystemMetrics(SM_CXSMICON),
                                 GetSystemMetrics(SM_CYSMICON), 0);
    if (app.icon) {
        app.iconOwned = true;
    } else {
        app.icon = LoadIconW(nullptr, IDI_APPLICATION);
        app.iconOwned = false;
    }

    g_app = &app;

    // 自检模式不受单实例限制；正常模式在注册窗口类之前就判断，第二次启动开销最小
    if (!selfTest) {
        app.mutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (app.mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND other = FindWindowW(kWndClass, nullptr);
            if (other) PostMessageW(other, WM_APP_ACTIVATE, 0, 0);
            CloseHandle(app.mutex);
            g_app = nullptr;
            return 0;
        }
        // 不主动 ReleaseMutex：进程退出时系统释放所有权；句柄在收尾时关闭
    }

    if (!RegisterMainClass(app)) {
        if (app.mutex) CloseHandle(app.mutex);
        if (app.iconOwned && app.icon) DestroyIcon(app.icon);
        g_app = nullptr;
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        // NOACTIVATE：点卡片不再抢走你正在打的字；拖动、右键菜单、双击刷新都不受影响
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        kWndClass, L"ip-float",
        WS_POPUP,
        0, 0, ScalePx(340, 1.0f), ScalePx(66, 1.0f),
        nullptr, nullptr, app.inst, nullptr);
    if (!hwnd) {
        if (app.mutex) CloseHandle(app.mutex);
        if (app.iconOwned && app.icon) DestroyIcon(app.icon);
        g_app = nullptr;
        return 1;
    }
    app.hwnd = hwnd;

    ApplyLayout(app, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);

    TrayInit(app.tray, hwnd, app.icon, WM_APP_TRAY, L"ip-float — 出口 IP");
    if (!TrayAdd(app.tray)) {
        app.trayRetries = kTrayRetries;
        SetTimer(hwnd, ID_TIMER_TRAY_RETRY, kTrayRetryMs, nullptr);
    }

    if (opt.demo) {
        selftest::DemoCard d = selftest::MakeDemoCard(NowFileTime());
        app.info = d.info;
        app.updatedAt = d.updatedAt;
        app.hasInfo = true;
        InvalidateRect(hwnd, nullptr, FALSE);

        if (!opt.shotPath.empty()) {
            app.shotDone = true;
            SetTimer(hwnd, ID_TIMER_SHOT, 700, nullptr);
        }
        if (!opt.dlgShotPath.empty()) {
            // 自检：设置窗口内部会定时拍图并自己关闭
            SettingsDialogInput in;
            in.cfg = app.cfg;
            in.scale = app.scale;
            in.shotPath = opt.dlgShotPath;
            SettingsDialogResult res;
            ShowSettingsDialog(hwnd, in, res);
            PostQuitMessage(0);
        }
    } else {
        StartFetch(app);
        ResetRefreshTimer(app);
        SetTimer(hwnd, ID_TIMER_TICK, 30000, nullptr);
        // 上次退出时诊断窗口是开着的 —— 等第一轮抓取完成再开，
        // 否则会先弹出一个只有「结论」两行的空窗口，几秒后再跳成完整高度
        if (app.cfg.panelOpen) app.pendingPanelOpen = true;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    TrayRemove(app.tray);
    if (app.mutex) {
        CloseHandle(app.mutex);
        app.mutex = nullptr;
    }
    if (app.iconOwned && app.icon) {
        DestroyIcon(app.icon);
        app.icon = nullptr;
    }
    g_app = nullptr;
    return 0;
}
