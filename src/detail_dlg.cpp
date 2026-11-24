#include "detail_dlg.h"

#include "card.h"
#include "util.h"

#include <windows.h>
#include <dwmapi.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

const wchar_t* kClass = L"ip-float-diag";

const int ID_BTN_REFRESH = 2001;
const int ID_BTN_COPY    = 2002;
const int ID_BTN_CLOSE   = 2003;

const COLORREF kBg     = RGB(38, 40, 45);
const COLORREF kText   = RGB(232, 234, 237);
const COLORREF kDim    = RGB(150, 156, 166);
const COLORREF kBorder = RGB(60, 65, 75);
const COLORREF kBtnBg  = RGB(50, 53, 60);
const COLORREF kAccent = RGB(120, 170, 255);
const COLORREF kOk     = RGB(158, 206, 106);
const COLORREF kWarn   = RGB(224, 175, 104);
const COLORREF kBad    = RGB(247, 118, 142);
const COLORREF kSkip   = RGB(120, 126, 136);

struct Row {
    int          y = 0, h = 0, x = 0, w = 0;
    HFONT        font = nullptr;
    COLORREF     color = kText;
    bool         hasDot = false;
    bool         hollow = false;   // 空心点 = 不适用
    COLORREF     dot = kSkip;
    bool         wrap = false;
    std::wstring text;
};

struct DiagWin {
    HWND hwnd = nullptr;
    HWND app = nullptr;
    HWND btnRefresh = nullptr;
    HWND btnCopy = nullptr;
    HWND btnClose = nullptr;

    HFONT base = nullptr, small = nullptr, strong = nullptr;
    HBRUSH bgBrush = nullptr;
    HICON  blankIcon = nullptr;

    float      scale = 1.0f;
    Diagnosis  diag;
    uint64_t   at = 0;
    bool       busy = false;

    std::vector<Row> rows;
    int pad = 0, gap = 0, gapSmall = 0, labelW = 0, dotD = 0;
    int baseH = 0, smallH = 0;
    int btnH = 0, btnW = 0;
    int contentW = 0, btnTop = 0, clientH = 0;
};

DiagWin* g_win = nullptr;

HFONT MakeFont(int px, bool bold)
{
    return CreateFontW(-px, 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

int TextLineH(HDC dc, HFONT f)
{
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    return tm.tmHeight;
}

int MeasureWrapped(HDC dc, HFONT f, const std::wstring& t, int w)
{
    if (t.empty() || w <= 0) return 0;
    HGDIOBJ old = SelectObject(dc, f);
    RECT r = { 0, 0, w, 0 };
    DrawTextW(dc, t.c_str(), -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    return r.bottom - r.top;
}

void DrawTextIn(HDC dc, HFONT f, COLORREF col, const RECT& rc, const std::wstring& s, UINT flags)
{
    if (s.empty()) return;
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    RECT r = rc;
    DrawTextW(dc, s.c_str(), -1, &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}

COLORREF SevCol(Sev s)
{
    switch (s) {
        case Sev::Ok:   return kOk;
        case Sev::Warn: return kWarn;
        case Sev::Bad:  return kBad;
        default:        return kSkip;
    }
}

int GroupOf(LineId id)
{
    switch (id) {
        case LineId::Tunnel:
        case LineId::V6Route:
        case LineId::Proxy:   return 0;
        case LineId::ExitV4:
        case LineId::ExitV6:
        case LineId::Asn:     return 1;
        default:              return 2;
    }
}

const wchar_t* GroupTitle(int g)
{
    return g == 0 ? L"隧道" : (g == 1 ? L"出口" : L"一致性");
}

void FreeFonts(DiagWin& s)
{
    if (s.base)   { DeleteObject(s.base);   s.base = nullptr; }
    if (s.small)  { DeleteObject(s.small);  s.small = nullptr; }
    if (s.strong) { DeleteObject(s.strong); s.strong = nullptr; }
}

void BuildFonts(DiagWin& s)
{
    FreeFonts(s);
    s.base   = MakeFont(ScalePx(15, s.scale), false);
    s.small  = MakeFont(ScalePx(13, s.scale), false);
    s.strong = MakeFont(ScalePx(15, s.scale), true);
}

void LayoutRows(DiagWin& s, HDC dc)
{
    s.pad      = ScalePx(24, s.scale);
    s.gap      = ScalePx(16, s.scale);
    s.gapSmall = ScalePx(7, s.scale);
    s.labelW   = ScalePx(92, s.scale);
    s.dotD     = ScalePx(7, s.scale);
    s.btnH     = ScalePx(30, s.scale);
    s.btnW     = ScalePx(76, s.scale);
    s.contentW = ScalePx(460, s.scale);

    s.baseH  = TextLineH(dc, s.base);
    s.smallH = TextLineH(dc, s.small);


    s.rows.clear();
    const int W = s.contentW;
    const int textX = s.pad + s.dotD + s.gapSmall;
    int y = s.pad;

    // 结论
    {
        Row r;
        r.y = y; r.h = s.baseH; r.x = textX; r.w = W - s.pad - textX;
        r.font = s.base; r.color = kText;
        r.text = s.diag.summary.empty() ? L"尚未检测" : s.diag.summary;
        r.hasDot = true;
        r.dot = SevCol(s.diag.verdict);
        s.rows.push_back(r);
        y += s.baseH + ScalePx(3, s.scale);
    }
    // 依据
    if (!s.diag.evidence.empty()) {
        Row r;
        r.y = y; r.x = textX; r.w = W - s.pad - textX;
        r.font = s.small; r.color = kDim; r.text = s.diag.evidence; r.wrap = true;
        r.h = MeasureWrapped(dc, r.font, r.text, r.w);
        s.rows.push_back(r);
        y += r.h;
    }
    y += s.gap;

    // 还没有数据（首次打开且抓取未完成）：给一行占位，别显示一个空壳
    if (s.diag.lines.empty()) {
        Row r;
        r.y = y; r.h = s.baseH; r.x = textX;
        r.w = W - s.pad - r.x;
        r.font = s.base; r.color = kDim; r.text = L"正在检测…";
        s.rows.push_back(r);
        y += s.baseH;
    }

    // 分组
    int curGroup = -1;
    for (const auto& l : s.diag.lines) {
        const int g = GroupOf(l.id);
        if (g != curGroup) {
            if (curGroup != -1) y += ScalePx(6, s.scale);
            Row r;
            r.y = y; r.h = s.smallH; r.x = s.pad; r.w = W - s.pad * 2;
            r.font = s.small; r.color = kAccent; r.text = GroupTitle(g);
            s.rows.push_back(r);
            y += s.smallH + ScalePx(3, s.scale);
            curGroup = g;
        }

        Row lab;
        lab.y = y; lab.h = s.baseH; lab.x = textX; lab.w = s.labelW;
        lab.font = s.base; lab.color = kDim; lab.text = l.label;
        lab.hasDot = true; lab.dot = SevCol(l.sev); lab.hollow = l.na;
        s.rows.push_back(lab);

        Row val;
        val.y = y; val.h = s.baseH;
        val.x = textX + s.labelW; val.w = W - s.pad - val.x;
        val.font = s.strong; val.color = kText; val.text = l.value;
        s.rows.push_back(val);
        y += s.baseH;

        if (!l.note.empty()) {
            Row n;
            n.y = y; n.x = val.x; n.w = val.w;
            n.font = s.small; n.color = kDim; n.text = l.note; n.wrap = true;
            n.h = MeasureWrapped(dc, n.font, n.text, n.w);
            s.rows.push_back(n);
            y += n.h;
        }
        y += s.gapSmall;
    }

    // 底部两行说明
    y += s.gap / 2;
    {
        Row r;
        r.y = y; r.h = s.smallH; r.x = s.pad; r.w = W - s.pad * 2;
        r.font = s.small; r.color = kDim;
        r.text = s.at ? (L"上次检测 " + LocalTimeString(s.at)) : std::wstring(L"尚未检测");
        s.rows.push_back(r);
        y += s.smallH + ScalePx(2, s.scale);
    }
    {
        Row r;
        r.y = y; r.h = s.smallH; r.x = s.pad; r.w = W - s.pad * 2;
        r.font = s.small; r.color = kSkip;
        r.text = L"只能看本机出口；看不到浏览器指纹与账号侧信号";
        s.rows.push_back(r);
        y += s.smallH;
    }

    y += s.gap;
    s.btnTop = y;
    y += s.btnH + s.pad / 2;
    s.clientH = y;
}

// 一次算清所有行与客户端尺寸；绘制只按算好的坐标画，不再做任何测量
void Rebuild(DiagWin& s)
{
    HDC dc = GetDC(s.hwnd);
    if (!dc) return;
    BuildFonts(s);
    LayoutRows(s, dc);
    ReleaseDC(s.hwnd, dc);
}

void LayoutButtons(DiagWin& s)
{
    const int right = s.contentW - s.pad;
    const int top = s.btnTop;
    auto move = [&](HWND h, int x) {
        if (h) MoveWindow(h, x, top, s.btnW, s.btnH, TRUE);
    };
    move(s.btnClose,   right - s.btnW);
    move(s.btnCopy,    right - s.btnW * 2 - ScalePx(10, s.scale));
    move(s.btnRefresh, right - s.btnW * 3 - ScalePx(20, s.scale));
}

void ApplyClientSize(DiagWin& s)
{
    RECT rc = { 0, 0, s.contentW, s.clientH };
    DWORD style = (DWORD)GetWindowLongPtrW(s.hwnd, GWL_STYLE);
    DWORD ex = (DWORD)GetWindowLongPtrW(s.hwnd, GWL_EXSTYLE);
    AdjustWindowRectEx(&rc, style, FALSE, ex);
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;

    // 贴着卡片放，放不下就挪到左边
    RECT card = {};
    GetWindowRect(s.app, &card);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(MonitorFromWindow(s.app, MONITOR_DEFAULTTONEAREST), &mi);

    const int gap = ScalePx(12, s.scale);
    // 贴卡片右侧；右边放不下就放左侧；都不行就贴工作区左边缘。
    // 全程夹在工作区内，绝不跨出屏幕。
    int x = card.right + gap;
    if (x + w > mi.rcWork.right) x = card.left - w - gap;
    if (x < mi.rcWork.left)      x = mi.rcWork.left;
    int y = card.top;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (y < mi.rcWork.top)        y = mi.rcWork.top;

    SetWindowPos(s.hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

const wchar_t* SevW(Sev s)
{
    switch (s) {
        case Sev::Ok:   return L"OK";
        case Sev::Warn: return L"WARN";
        case Sev::Bad:  return L"BAD";
        default:        return L"--";
    }
}

std::wstring BuildPlainText(const DiagWin& s)
{
    std::wstring t = L"ip-float 网络诊断\n";
    if (s.at) t += L"上次检测 " + LocalTimeString(s.at) + L"\n";
    t += L"\n";
    t += L"结论 [" + std::wstring(SevW(s.diag.verdict)) + L"] " + s.diag.summary + L"\n";
    if (!s.diag.evidence.empty()) t += L"依据 " + s.diag.evidence + L"\n";
    t += L"\n";

    int cur = -1;
    for (const auto& l : s.diag.lines) {
        const int g = GroupOf(l.id);
        if (g != cur) {
            t += std::wstring(L"── ") + GroupTitle(g) + L" ──\n";
            cur = g;
        }
        std::wstring tag = std::wstring(L"[") + SevW(l.sev) + L"]";
        while (tag.size() < 7) tag += L' ';
        t += tag + l.label + L"：" + l.value;
        if (!l.note.empty()) t += L"（" + l.note + L"）";
        t += L"\n";
    }
    return t;
}

void CopyToClipboard(HWND owner, const std::wstring& text)
{
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* p = GlobalLock(mem);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        } else {
            GlobalFree(mem);
        }
    }
    CloseClipboard();
}

// 行绘制：窗口和离屏渲染共用同一段代码，避免两处画得不一样
void PaintContent(HDC mem, const DiagWin& s)
{
    for (const auto& r : s.rows) {
        if (r.hasDot) {
            const int d = s.dotD;
            const int cy = r.y + r.h / 2 - d / 2;
            if (r.hollow) {
                HPEN ring = CreatePen(PS_SOLID, 1, r.dot);
                HGDIOBJ oldPen = SelectObject(mem, ring);
                HGDIOBJ oldBr = SelectObject(mem, GetStockObject(NULL_BRUSH));
                Ellipse(mem, s.pad, cy, s.pad + d + 1, cy + d + 1);
                SelectObject(mem, oldBr);
                SelectObject(mem, oldPen);
                DeleteObject(ring);
            } else {
                HBRUSH br = CreateSolidBrush(r.dot);
                HPEN pen = CreatePen(PS_NULL, 0, RGB(0, 0, 0));
                HGDIOBJ oldBr = SelectObject(mem, br);
                HGDIOBJ oldPen = SelectObject(mem, pen);
                Ellipse(mem, s.pad, cy, s.pad + d + 1, cy + d + 1);
                SelectObject(mem, oldBr);
                SelectObject(mem, oldPen);
                DeleteObject(br);
                DeleteObject(pen);
            }
        }
        RECT tr = { r.x, r.y, r.x + r.w, r.y + (r.h > 0 ? r.h : 0) };
        UINT flags = DT_LEFT | DT_TOP |
                     (r.wrap ? DT_WORDBREAK : (DT_SINGLELINE | DT_END_ELLIPSIS));
        DrawTextIn(mem, r.font ? r.font : s.base, r.color, tr, r.text, flags);
    }
}

// 离屏渲染里的三个按钮：和窗口里的 owner-draw 长得一样
void PaintStaticButtons(HDC dc, const DiagWin& s)
{
    const int right = s.contentW - s.pad;
    const int gap = ScalePx(10, s.scale);
    const wchar_t* labels[3] = { L"刷新", L"复制", L"关闭" };
    const int xs[3] = { right - s.btnW * 3 - gap * 2,
                        right - s.btnW * 2 - gap,
                        right - s.btnW };
    for (int i = 0; i < 3; ++i) {
        RECT rc = { xs[i], s.btnTop, xs[i] + s.btnW, s.btnTop + s.btnH };
        HBRUSH bg = CreateSolidBrush(kBtnBg);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        HBRUSH bd = CreateSolidBrush(kBorder);
        FrameRect(dc, &rc, bd);
        DeleteObject(bd);
        DrawTextIn(dc, s.base, kText, rc, labels[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void PaintButton(const DRAWITEMSTRUCT* dis)
{
    if (!dis || !g_win) return;
    HDC dc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;

    HBRUSH br = CreateSolidBrush(pressed ? RGB(64, 68, 78) : kBtnBg);
    FillRect(dc, &rc, br);
    DeleteObject(br);

    HBRUSH border = CreateSolidBrush(kBorder);
    FrameRect(dc, &rc, border);
    DeleteObject(border);

    wchar_t label[64] = {};
    GetWindowTextW(dis->hwndItem, label, 64);
    DrawTextIn(dc, g_win->base, disabled ? kSkip : kText, rc, label,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void CreateButtons(DiagWin& s)
{
    auto make = [&](const wchar_t* text, int id) {
        return CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               0, 0, 10, 10, s.hwnd, (HMENU)(INT_PTR)id,
                               GetModuleHandleW(nullptr), nullptr);
    };
    s.btnRefresh = make(L"刷新", ID_BTN_REFRESH);
    s.btnCopy    = make(L"复制", ID_BTN_COPY);
    s.btnClose   = make(L"关闭", ID_BTN_CLOSE);
    LayoutButtons(s);
}

void UpdateBusyState(DiagWin& s)
{
    if (s.btnRefresh) EnableWindow(s.btnRefresh, s.busy ? FALSE : TRUE);
    if (s.btnRefresh) SetWindowTextW(s.btnRefresh, s.busy ? L"检测中…" : L"刷新");
}

void Repaint(DiagWin& s, bool relayout)
{
    if (!s.hwnd) return;
    if (relayout) {
        Rebuild(s);
        LayoutButtons(s);
        ApplyClientSize(s);
    }
    UpdateBusyState(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK DiagProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    DiagWin* s = g_win;
    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        s = (DiagWin*)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)s);
        s->hwnd = h;
        return 0;
    }
    if (!s) return DefWindowProcW(h, msg, wp, lp);

    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
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

            PaintContent(mem, *s);

            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, oldBmp);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(h, &ps);
            return 0;
        }

        case WM_DRAWITEM:
            PaintButton((DRAWITEMSTRUCT*)lp);
            return TRUE;

        case WM_COMMAND: {
            const int id = LOWORD(wp);
            if (id == ID_BTN_REFRESH) {
                if (s->app) PostMessageW(s->app, WM_DIAG_REFRESH, 0, 0);
            } else if (id == ID_BTN_COPY) {
                CopyToClipboard(h, BuildPlainText(*s));
            } else if (id == ID_BTN_CLOSE) {
                DestroyWindow(h);
            }
            return 0;
        }

        case WM_DPICHANGED: {
            s->scale = (float)HIWORD(wp) / 96.0f;
            if (s->scale <= 0.0f) s->scale = 1.0f;
            const RECT* sug = (const RECT*)lp;
            if (sug) SetWindowPos(h, nullptr, sug->left, sug->top, 0, 0,
                                  SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            Repaint(*s, true);
            return 0;
        }

        case WM_CLOSE:
            DestroyWindow(h);
            return 0;

        case WM_DESTROY: {
            const HWND app = s->app;
            FreeFonts(*s);
            if (s->bgBrush) DeleteObject(s->bgBrush);
            if (s->blankIcon) DestroyIcon(s->blankIcon);
            g_win = nullptr;
            delete s;
            if (app) PostMessageW(app, WM_DIAG_CLOSED, 0, 0);
            return 0;
        }

        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 标题栏左侧那个位置系统一定会画东西：把 exe 的图标缩到 16x16 通常很糊。
// 这里造一个全透明图标，等于让它只留背景色。
HICON CreateBlankIcon(int size)
{
    BITMAPV5HEADER bi = {};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = size;
    bi.bV5Height      = -size;          // 负数 = 自上而下
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP color = CreateDIBSection(screen, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color) return nullptr;
    if (bits) memset(bits, 0, (size_t)size * size * 4);   // alpha 全 0

    std::vector<BYTE> maskBits(((size_t)(size + 15) / 16) * 2 * size, 0xFF);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, maskBits.data());

    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmMask  = mask;
    ii.hbmColor = color;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

void RegisterClassOnce()
{
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DiagProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    done = true;
}

} // namespace

void ShowDiagWindow(HWND appHwnd, const Diagnosis& d, uint64_t at)
{
    if (g_win) {
        UpdateDiagWindow(d, at);
        SetForegroundWindow(g_win->hwnd);
        return;
    }

    RegisterClassOnce();

    auto* s = new DiagWin();
    s->app = appHwnd;
    s->diag = d;
    s->at = at;
    s->bgBrush = CreateSolidBrush(kBg);

    HMONITOR mon = MonitorFromWindow(appHwnd, MONITOR_DEFAULTTONEAREST);
    s->scale = DpiScaleForMonitor(mon);
    if (s->scale <= 0.0f) s->scale = 1.0f;

    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    HWND h = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"网络诊断", style,
                             0, 0, ScalePx(500, s->scale), ScalePx(600, s->scale),
                             nullptr, nullptr, GetModuleHandleW(nullptr), s);
    if (!h) {
        if (s->bgBrush) DeleteObject(s->bgBrush);
        delete s;
        return;
    }
    g_win = s;

    // 标题栏图标换成全透明的，别让缩小的 exe 图标在那儿糊着
    {
        const int px = GetSystemMetrics(SM_CXSMICON);
        s->blankIcon = CreateBlankIcon(px > 0 ? px : 16);
        if (s->blankIcon) {
            SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)s->blankIcon);
            SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)s->blankIcon);
        }
    }

    // 深色标题栏，跟内容区一致（Win11 用 20，旧版本用 19）
    {
        BOOL dark = TRUE;
        if (FAILED(DwmSetWindowAttribute(h, 20, &dark, sizeof(dark))))
            DwmSetWindowAttribute(h, 19, &dark, sizeof(dark));
    }

    Rebuild(*s);
    CreateButtons(*s);
    ApplyClientSize(*s);
    UpdateBusyState(*s);
    ShowWindow(h, SW_SHOWNOACTIVATE);
    InvalidateRect(h, nullptr, FALSE);
}

void UpdateDiagWindow(const Diagnosis& d, uint64_t at)
{
    if (!g_win) return;
    g_win->diag = d;
    g_win->at = at;
    g_win->busy = false;
    Repaint(*g_win, true);
}

void SetDiagBusy(bool busy)
{
    if (!g_win) return;
    g_win->busy = busy;
    UpdateBusyState(*g_win);
}

bool DiagWindowOpen()
{
    return g_win != nullptr;
}

void CloseDiagWindow()
{
    if (!g_win) return;
    HWND h = g_win->hwnd;
    g_win->app = nullptr;      // 别再回发 WM_DIAG_CLOSED，进程正在退出
    DestroyWindow(h);
}

// -mwindows 没有控制台：自检结果只能落在图片旁边（UTF-16LE，和 card.cpp 的格式一致）
void WriteSelfTestLog(const std::wstring& imagePath, const std::wstring& msg)
{
    HANDLE f = CreateFileW((imagePath + L".log").c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, msg.c_str(), (DWORD)(msg.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(f);
}

bool RenderDiagToPng(const std::wstring& path, float scale, const Diagnosis& d, uint64_t at,
                     std::wstring& err)
{
    if (scale <= 0.0f) scale = 2.0f;

    DiagWin s;
    s.scale = scale;
    s.diag = d;
    s.at = at;

    HDC screen = GetDC(nullptr);
    if (!screen) {
        err = L"取屏幕 DC 失败";
        WriteSelfTestLog(path, L"render-dlg FAILED: " + err);
        return false;
    }
    BuildFonts(s);
    LayoutRows(s, screen);
    ReleaseDC(nullptr, screen);

    Gdiplus::Bitmap bmp(s.contentW, s.clientH, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Gdiplus::Ok) {
        FreeFonts(s);
        err = L"创建位图失败";
        WriteSelfTestLog(path, L"render-dlg FAILED: " + err);
        return false;
    }

    Gdiplus::Graphics g(&bmp);
    HDC mem = g.GetHDC();
    if (!mem) {
        FreeFonts(s);
        err = L"取位图 DC 失败";
        WriteSelfTestLog(path, L"render-dlg FAILED: " + err);
        return false;
    }

    RECT rc = { 0, 0, s.contentW, s.clientH };
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);

    SetBkMode(mem, TRANSPARENT);
    PaintContent(mem, s);
    PaintStaticButtons(mem, s);

    g.ReleaseHDC(mem);
    FreeFonts(s);

    const bool ok = SavePngToFile(bmp, path, err);
    WriteSelfTestLog(path, ok ? L"rendered" : (L"render-dlg FAILED: " + err));
    return ok;
}
