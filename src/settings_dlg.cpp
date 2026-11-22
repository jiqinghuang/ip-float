#include "settings_dlg.h"

#include "card.h"
#include "util.h"

#include <gdiplus.h>

using namespace Gdiplus;

namespace {

const wchar_t* kDlgClass = L"ip-float-settings";

const UINT ID_TIMER_DLGSHOT = 4;

const int ID_EDIT_INTERVAL = 1001;
const int ID_CHK_TOPMOST   = 1002;
const int ID_CHK_AUTOSTART = 1003;
const int ID_EDIT_OPACITY  = 1004;

const Color kFieldBg  (255,  50,  53,  60);
const Color kDlgBg    (255,  38,  40,  45);
const Color kBorderC  (255,  60,  65,  75);
const Color kTextC    (255, 232, 234, 237);
const Color kDimC     (255, 150, 156, 166);
const Color kAccent   (255,  82, 139, 255);
const Color kAccentLo (255,  56,  92, 168);

// 一行「标签 + 圆角输入区 + 单位 + 区间提示」，所有 rect 同源计算
struct DlgField {
    RECT label = {};
    RECT box   = {};
    RECT edit  = {};
    RECT unit  = {};
    RECT hint  = {};
};

// 对话框的状态与资源都挂在这里，不再用 g_dlg / g_dm 全局。
struct DlgState {
    HWND hwnd = nullptr;
    HWND edit = nullptr;
    HWND editOpacity = nullptr;
    HWND chkTop = nullptr;
    HWND chkAuto = nullptr;
    HWND btnOk = nullptr;
    HWND btnCancel = nullptr;

    bool done = false;
    bool ok = false;

    int  interval = 5;
    int  opacity = 94;
    int  initialInterval = 5;
    int  initialOpacity = 94;
    int  focus = -1;

    bool topMost = true;
    bool autoStart = false;

    float scale = 1.0f;
    std::wstring shotPath;

    HFONT  base = nullptr;
    HFONT  small = nullptr;
    HBRUSH fieldBrush = nullptr;
    HBRUSH bgBrush = nullptr;

    int baseH = 0, smallH = 0;
    int pad = 0, gap = 0, box = 0;
    int fieldH = 0, checkH = 0, btnH = 0, btnW = 0;
    int editW = 0, inset = 0;
    int clientW = 0, clientH = 0;

    DlgField fInterval = {};
    DlgField fOpacity = {};
    RECT noteRc = {};
    RECT chkTopRc = {};
    RECT chkAutoRc = {};
    RECT btnOkRc = {};
    RECT btnCancelRc = {};

    ~DlgState()
    {
        if (base) DeleteObject(base);
        if (small) DeleteObject(small);
        if (fieldBrush) DeleteObject(fieldBrush);
        if (bgBrush) DeleteObject(bgBrush);
    }
};

HFONT DlgFont(int px)
{
    return CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

// 度量与绘制共用同一 HFONT，保证「量出来的宽度」就是「画出来的宽度」
int DlgTextW(HDC dc, HFONT f, const wchar_t* s)
{
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

int DlgLineH(HDC dc, HFONT f)
{
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    return tm.tmHeight;
}

void DlgTextIn(HDC dc, HFONT f, COLORREF col, const RECT& rc,
               const wchar_t* s, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, col);
    RECT r = rc;
    DrawTextW(dc, s, -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

// 一次算清所有尺寸与位置：clientW/clientH 与控件 rect 同源，不会互相打架
void DlgBuildMetrics(DlgState& s, HDC dc)
{
    s.base = DlgFont(ScalePx(15, s.scale));
    s.small = DlgFont(ScalePx(13, s.scale));
    s.baseH = DlgLineH(dc, s.base);
    s.smallH = DlgLineH(dc, s.small);

    s.pad = ScalePx(24, s.scale);
    s.gap = ScalePx(12, s.scale);
    s.box = ScalePx(18, s.scale);
    s.fieldH = s.baseH + ScalePx(18, s.scale);
    s.checkH = s.baseH + ScalePx(10, s.scale);
    s.btnH = s.baseH + ScalePx(16, s.scale);
    s.btnW = ScalePx(94, s.scale);
    s.editW = ScalePx(64, s.scale);
    s.inset = ScalePx(14, s.scale);

    s.fieldBrush = CreateSolidBrush(RGB(50, 53, 60));
    s.bgBrush = CreateSolidBrush(RGB(38, 40, 45));

    int boxText = s.box + ScalePx(12, s.scale);
    int w = s.pad * 2 + ScalePx(300, s.scale);
    int c;

    c = s.pad * 2 + boxText + DlgTextW(dc, s.base, L"开机时自动启动");
    if (c > w) w = c;
    c = s.pad * 2 + s.btnW * 2 + s.gap;
    if (c > w) w = c;
    c = s.pad * 2 + boxText + DlgTextW(dc, s.small, L"登录 Windows 时自动运行");
    if (c > w) w = c;
    c = s.pad * 2 + DlgTextW(dc, s.base, L"不透明度");
    if (c > w) w = c;
    s.clientW = w;

    const int pad = s.pad;
    const int cw = s.clientW;
    int y = pad;

    auto field = [&](DlgField& f, const wchar_t* hint) {
        f.label = RECT{ pad, y, cw - pad, y + s.baseH };
        y += s.baseH + ScalePx(8, s.scale);
        f.box = RECT{ pad, y, cw - pad, y + s.fieldH };
        y += s.fieldH + ScalePx(18, s.scale);

        const int editH = s.baseH + ScalePx(6, s.scale);
        const int et = f.box.top + (s.fieldH - editH) / 2;
        f.edit = RECT{ f.box.left + s.inset, et,
                       f.box.left + s.inset + s.editW, et + editH };
        f.unit = RECT{ f.edit.right + ScalePx(10, s.scale),
                       f.box.top + (s.fieldH - s.baseH) / 2,
                       f.edit.right + ScalePx(10, s.scale) + ScalePx(120, s.scale),
                       f.box.top + (s.fieldH - s.baseH) / 2 + s.baseH };

        const int hw = DlgTextW(dc, s.small, hint);
        f.hint = RECT{ f.box.right - s.inset - hw,
                       f.box.top + (s.fieldH - s.smallH) / 2,
                       f.box.right - s.inset,
                       f.box.top + (s.fieldH - s.smallH) / 2 + s.smallH };
    };

    field(s.fInterval, L"1 - 1440");
    field(s.fOpacity, L"0 - 100");

    s.chkTopRc = { pad, y, cw - pad, y + s.checkH };
    y += s.checkH + s.gap;

    s.chkAutoRc = { pad, y, cw - pad, y + s.checkH };
    y += s.checkH + ScalePx(4, s.scale);

    s.noteRc = { pad + boxText, y, cw - pad, y + s.smallH };
    y += s.smallH + ScalePx(22, s.scale);

    s.btnOkRc = { cw - pad - s.btnW, y, cw - pad, y + s.btnH };
    s.btnCancelRc = { cw - pad - s.btnW * 2 - s.gap, y,
                      cw - pad - s.btnW - s.gap, y + s.btnH };
    y += s.btnH + pad;
    s.clientH = y;
}

void DlgCreateChildren(DlgState& s, HWND h)
{
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                    const RECT& r, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                               r.left, r.top, r.right - r.left, r.bottom - r.top,
                               h, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    };

    wchar_t buf[16] = {};
    swprintf(buf, 16, L"%d", s.interval);

    s.edit = make(L"EDIT", buf, ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER,
                  s.fInterval.edit, ID_EDIT_INTERVAL);
    SendMessageW(s.edit, WM_SETFONT, (WPARAM)s.base, TRUE);

    swprintf(buf, 16, L"%d", s.opacity);
    s.editOpacity = make(L"EDIT", buf, ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER,
                         s.fOpacity.edit, ID_EDIT_OPACITY);
    SendMessageW(s.editOpacity, WM_SETFONT, (WPARAM)s.base, TRUE);

    s.chkTop = make(L"BUTTON", L"窗口总在最前", BS_OWNERDRAW, s.chkTopRc, ID_CHK_TOPMOST);
    s.chkAuto = make(L"BUTTON", L"开机时自动启动", BS_OWNERDRAW, s.chkAutoRc, ID_CHK_AUTOSTART);
    s.btnOk = make(L"BUTTON", L"确定", BS_OWNERDRAW, s.btnOkRc, IDOK);
    s.btnCancel = make(L"BUTTON", L"取消", BS_OWNERDRAW, s.btnCancelRc, IDCANCEL);

    SetFocus(s.edit);
}

// 空输入框按“保持原值”处理：否则不小心清空 + 确定，会把卡片直接变成 0% 透明
int ReadIntField(HWND edit, int fallback, int lo, int hi)
{
    wchar_t buf[32] = {};
    if (!edit) return fallback;
    GetWindowTextW(edit, buf, 32);
    if (buf[0] == L'\0') return fallback;
    int v = _wtoi(buf);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

LRESULT CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    DlgState* s = (DlgState*)GetWindowLongPtrW(h, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        s = (DlgState*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);
        s->hwnd = h;
        DlgCreateChildren(*s, h);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_CTLCOLOREDIT:
        if (!s) break;
        SetTextColor((HDC)wp, RGB(232, 234, 237));
        SetBkColor((HDC)wp, RGB(50, 53, 60));
        return (LRESULT)s->fieldBrush;

    case WM_PAINT: {
        if (!s) break;
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc = {};
        GetClientRect(h, &rc);

        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        if (!mem || !bmp) {
            if (bmp) DeleteObject(bmp);
            if (mem) DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        FillRect(mem, &rc, s->bgBrush);

        auto drawField = [&](const DlgField& f, bool focused) {
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            REAL fw = (REAL)(f.box.right - f.box.left);
            REAL fh = (REAL)(f.box.bottom - f.box.top);
            GraphicsPath p;
            RoundedPath(p, (REAL)f.box.left + 0.5f, (REAL)f.box.top + 0.5f,
                        fw - 1.0f, fh - 1.0f, (REAL)ScalePx(9, s->scale));
            SolidBrush fill(kFieldBg);
            g.FillPath(&fill, &p);
            Pen pen(focused ? kAccent : kBorderC, 1.0f);
            g.DrawPath(&pen, &p);
        };
        drawField(s->fInterval, s->focus == 0);
        drawField(s->fOpacity, s->focus == 1);

        SetBkMode(mem, TRANSPARENT);
        DlgTextIn(mem, s->base, RGB(232, 234, 237), s->fInterval.label, L"刷新间隔", DT_LEFT);
        DlgTextIn(mem, s->base, RGB(232, 234, 237), s->fOpacity.label, L"不透明度", DT_LEFT);
        DlgTextIn(mem, s->base, RGB(150, 156, 166), s->fInterval.unit, L"分钟", DT_LEFT);
        DlgTextIn(mem, s->base, RGB(150, 156, 166), s->fOpacity.unit, L"%", DT_LEFT);
        DlgTextIn(mem, s->small, RGB(150, 156, 166), s->fInterval.hint, L"1 - 1440", DT_RIGHT);
        DlgTextIn(mem, s->small, RGB(150, 156, 166), s->fOpacity.hint, L"0 - 100", DT_RIGHT);
        DlgTextIn(mem, s->small, RGB(118, 124, 134), s->noteRc,
                  L"登录 Windows 时自动运行", DT_LEFT);

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        if (!s) break;
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
        HDC dc = dis->hDC;
        RECT r = dis->rcItem;
        bool pressed = (dis->itemState & ODS_SELECTED) != 0;

        // BS_OWNERDRAW 的 BUTTON 会先用系统浅灰擦一遍，不重铺底色会留浅灰方块
        FillRect(dc, &r, s->bgBrush);

        if (dis->CtlID == ID_CHK_TOPMOST || dis->CtlID == ID_CHK_AUTOSTART) {
            bool on = (dis->CtlID == ID_CHK_TOPMOST) ? s->topMost : s->autoStart;
            int box = s->box;
            int top = r.top + (r.bottom - r.top - box) / 2;
            {
                Graphics g(dc);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                GraphicsPath p;
                RoundedPath(p, (REAL)r.left + 0.5f, (REAL)top + 0.5f,
                            (REAL)box - 1.0f, (REAL)box - 1.0f, (REAL)ScalePx(5, s->scale));
                if (on) {
                    SolidBrush b(pressed ? kAccentLo : kAccent);
                    g.FillPath(&b, &p);
                    Pen pen(Color(255, 255, 255, 255), (REAL)ScalePx(2, s->scale));
                    pen.SetStartCap(LineCapRound);
                    pen.SetEndCap(LineCapRound);
                    REAL bx = (REAL)r.left, by = (REAL)top, bs = (REAL)box;
                    g.DrawLine(&pen, bx + bs * 0.26f, by + bs * 0.54f,
                               bx + bs * 0.45f, by + bs * 0.72f);
                    g.DrawLine(&pen, bx + bs * 0.45f, by + bs * 0.72f,
                               bx + bs * 0.76f, by + bs * 0.28f);
                } else {
                    SolidBrush b(kFieldBg);
                    g.FillPath(&b, &p);
                    Pen pen(pressed ? kAccent : kBorderC, 1.0f);
                    g.DrawPath(&pen, &p);
                }
                if (dis->itemState & ODS_FOCUS) {
                    GraphicsPath fp;
                    RoundedPath(fp, (REAL)r.left - 2.5f, (REAL)top - 2.5f,
                                (REAL)box + 5.0f, (REAL)box + 5.0f,
                                (REAL)ScalePx(7, s->scale));
                    Pen fpen(Color(110, 82, 139, 255), 1.0f);
                    g.DrawPath(&fpen, &fp);
                }
            }
            wchar_t text[128] = {};
            GetWindowTextW(dis->hwndItem, text, 128);
            RECT tr = { r.left + box + ScalePx(12, s->scale), r.top, r.right, r.bottom };
            SetBkMode(dc, TRANSPARENT);
            DlgTextIn(dc, s->base, RGB(232, 234, 237), tr, text, DT_LEFT | DT_VCENTER);
        } else {
            bool primary = (dis->CtlID == IDOK);
            {
                Graphics g(dc);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                GraphicsPath p;
                RoundedPath(p, (REAL)r.left + 0.5f, (REAL)r.top + 0.5f,
                            (REAL)(r.right - r.left) - 1.0f,
                            (REAL)(r.bottom - r.top) - 1.0f,
                            (REAL)ScalePx(7, s->scale));
                if (primary) {
                    SolidBrush b(pressed ? kAccentLo : kAccent);
                    g.FillPath(&b, &p);
                } else {
                    SolidBrush b(kFieldBg);
                    g.FillPath(&b, &p);
                    Pen pen(pressed ? kAccent : kBorderC, 1.0f);
                    g.DrawPath(&pen, &p);
                }
            }
            wchar_t text[64] = {};
            GetWindowTextW(dis->hwndItem, text, 64);
            SetBkMode(dc, TRANSPARENT);
            DlgTextIn(dc, s->base, primary ? RGB(255, 255, 255) : RGB(232, 234, 237),
                      r, text, DT_CENTER | DT_VCENTER);
        }
        return TRUE;
    }

    case WM_COMMAND: {
        if (!s) break;
        int id = LOWORD(wp);
        UINT code = HIWORD(wp);

        if ((id == ID_EDIT_INTERVAL || id == ID_EDIT_OPACITY) &&
            (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
            if (code == EN_SETFOCUS) s->focus = (id == ID_EDIT_OPACITY) ? 1 : 0;
            else                     s->focus = -1;
            InvalidateRect(h, &s->fInterval.box, FALSE);
            InvalidateRect(h, &s->fOpacity.box, FALSE);
            return 0;
        }

        if (id == IDOK) {
            s->interval = ReadIntField(s->edit, s->initialInterval, 1, 1440);
            s->opacity = ReadIntField(s->editOpacity, s->initialOpacity, 0, 100);
            s->ok = true;
            s->done = true;
            DestroyWindow(h);
        } else if (id == IDCANCEL) {
            s->done = true;
            DestroyWindow(h);
        } else if (id == ID_CHK_TOPMOST) {
            s->topMost = !s->topMost;
            InvalidateRect(s->chkTop, nullptr, TRUE);
        } else if (id == ID_CHK_AUTOSTART) {
            s->autoStart = !s->autoStart;
            InvalidateRect(s->chkAuto, nullptr, TRUE);
        }
        return 0;
    }

    case WM_TIMER:
        if (s && wp == ID_TIMER_DLGSHOT) {
            KillTimer(h, ID_TIMER_DLGSHOT);
            std::wstring err;
            CaptureWindowToPng(h, s->shotPath, err);
            s->done = true;
            DestroyWindow(h);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (s) s->done = true;
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        if (s) {
            s->hwnd = nullptr;
            s->done = true;
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

bool ShowSettingsDialog(HWND parent, const SettingsDialogInput& in,
                        SettingsDialogResult& out)
{
    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kDlgClass;
        RegisterClassExW(&wc);
        classRegistered = true;
    }

    DlgState s;
    s.interval = in.cfg.intervalMinutes;
    s.opacity = in.cfg.opacityPercent;
    s.initialInterval = in.cfg.intervalMinutes;
    s.initialOpacity = in.cfg.opacityPercent;
    s.topMost = in.cfg.topMost;
    s.autoStart = in.cfg.autoStart;
    s.scale = in.scale;
    s.shotPath = in.shotPath;

    HDC sdc = GetDC(nullptr);
    DlgBuildMetrics(s, sdc);
    ReleaseDC(nullptr, sdc);

    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    RECT wr = { 0, 0, s.clientW, s.clientH };
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    RECT pr = {};
    GetWindowRect(parent, &pr);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &mi);

    int x = pr.left - winW - ScalePx(14, s.scale);
    if (x < mi.rcWork.left) x = pr.right + ScalePx(14, s.scale);
    if (x + winW > mi.rcWork.right) x = mi.rcWork.right - winW;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    int y = pr.top;
    if (y + winH > mi.rcWork.bottom) y = mi.rcWork.bottom - winH;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    HWND dlg = CreateWindowExW(exStyle, kDlgClass, L"ip-float 设置", style,
                               x, y, winW, winH, parent, nullptr,
                               GetModuleHandleW(nullptr), &s);
    if (!dlg) return false;
    s.hwnd = dlg;

    // 标题栏跟随深色（Win11 支持；老系统静默失败，不影响功能）
    typedef HRESULT(WINAPI* DwmSetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        DwmSetAttrFn fn = (DwmSetAttrFn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (fn) {
            BOOL dark = TRUE;
            COLORREF capBg = RGB(38, 40, 45);
            COLORREF capText = RGB(232, 234, 237);
            fn(dlg, 20, &dark, sizeof(dark));       // DWMWA_USE_IMMERSIVE_DARK_MODE
            fn(dlg, 35, &capBg, sizeof(capBg));     // DWMWA_CAPTION_COLOR
            fn(dlg, 36, &capText, sizeof(capText)); // DWMWA_TEXT_COLOR
        }
        FreeLibrary(dwm);
    }

    ShowWindow(dlg, SW_SHOW);
    SetFocus(s.edit);
    EnableWindow(parent, FALSE);
    SetForegroundWindow(dlg);

    if (!s.shotPath.empty()) SetTimer(dlg, ID_TIMER_DLGSHOT, 600, nullptr);

    bool quit = false;
    MSG msg;
    while (!s.done) {
        if (!GetMessageW(&msg, nullptr, 0, 0)) {
            quit = true;
            break;
        }
        if (!IsWindow(parent)) break;   // 父窗口没了，owned dialog 已被系统销毁
        if (msg.message == WM_KEYDOWN &&
            (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            SendMessageW(dlg, WM_COMMAND,
                         MAKEWPARAM(msg.wParam == VK_RETURN ? IDOK : IDCANCEL, BN_CLICKED), 0);
            continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // 嵌套循环吞掉 WM_QUIT 的话，外层 GetMessage 会永远阻塞，必须补回去
    if (quit) PostQuitMessage(0);

    if (IsWindow(parent)) EnableWindow(parent, TRUE);
    if (IsWindow(dlg)) DestroyWindow(dlg);
    if (IsWindow(parent)) SetForegroundWindow(parent);

    if (s.ok) {
        out.ok = true;
        out.interval = s.interval;
        out.opacity = s.opacity;
        out.topMost = s.topMost;
        out.autoStart = s.autoStart;
    }
    return s.ok;
}
