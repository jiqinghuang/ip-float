#include "card.h"

#include "util.h"

#include <cstring>
#include <cwchar>
#include <memory>
#include <vector>

using namespace Gdiplus;

namespace {

const Color kBg     (255,  30,  31,  34);
const Color kBorder (255,  60,  65,  75);
const Color kText   (255, 232, 234, 237);
const Color kDim    (255, 150, 156, 166);
const Color kWarn   (255, 224, 175, 104);
const Color kDotOk  (255, 158, 206, 106);
const Color kDotIdle(255, 150, 156, 166);
const Color kBad    (255, 247, 118, 142);   // 徽章「问题」色

const int BASE_W = 340;
const int BASE_H = 80;   // 原 66；多一行徽章 +14

Color SevColor(Sev s)
{
    switch (s) {
        case Sev::Ok:   return kDotOk;
        case Sev::Warn: return kWarn;
        case Sev::Bad:  return kBad;
        default:        return kDotIdle;   // Skip：没测/读不到，绝不能显示成通过
    }
}

std::unique_ptr<Font> MakeFont(float px)
{    auto f = std::make_unique<Font>(L"Microsoft YaHei UI", px, FontStyleRegular, UnitPixel);
    if (f->GetLastStatus() != Ok) {
        // 非中文系统可能没有雅黑：GDI+ 不像 GDI 那样自动 fallback，必须自己兜一下
        f = std::make_unique<Font>(L"Segoe UI", px, FontStyleRegular, UnitPixel);
    }
    return f;
}

int GetEncoderClsid(const WCHAR* mime, CLSID* clsid)
{
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> buf(size);
    ImageCodecInfo* info = (ImageCodecInfo*)buf.data();
    GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
        if (_wcsicmp(info[i].MimeType, mime) == 0) {
            *clsid = info[i].Clsid;
            return (int)i;
        }
    }
    return -1;
}

bool WriteAll(const std::wstring& path, const void* data, DWORD size)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(f, data, size, &written, nullptr);
    CloseHandle(f);
    return ok && written == size;
}

void WriteLog(const std::wstring& imagePath, const std::wstring& msg)
{
    // -mwindows 没有控制台，自检结果只能落在图片旁边
    std::wstring log = imagePath + L".log";
    WriteAll(log, msg.c_str(), (DWORD)(msg.size() * sizeof(wchar_t)));
}

void WriteSidecar(const std::wstring& pngPath, const CardMetrics& m)
{
    std::wstring json =
        L"{\n"
        L"  \"width\": " + std::to_wstring(m.width) + L",\n"
        L"  \"height\": " + std::to_wstring(m.height) + L",\n"
        L"  \"scale\": " + std::to_wstring(m.scale) + L",\n"
        L"  \"cornerRadius\": " + std::to_wstring(m.radius) + L",\n"
        L"  \"colors\": {\n"
        L"    \"bg\": \"#1E1F22\",\n"
        L"    \"border\": \"#3C414B\",\n"
        L"    \"text\": \"#E8EAED\",\n"
        L"    \"dim\": \"#969CA6\",\n"
        L"    \"warn\": \"#E0AF68\",\n"
        L"    \"dotOk\": \"#9ECE6A\",\n"
        L"    \"dotBad\": \"#F7768E\",\n"
        L"    \"dotIdle\": \"#969CA6\"\n"
        L"  }\n"
        L"}\n";

    std::string utf8 = WideToUtf8(json);
    WriteAll(pngPath + L".json", utf8.data(), (DWORD)utf8.size());
}

} // namespace

CardMetrics ComputeCardMetrics(float scale)
{
    CardMetrics m;
    m.scale = scale;
    m.width = ScalePx(BASE_W, scale);
    m.height = ScalePx(BASE_H, scale);

    m.dotX = ScalePx(11, scale);
    m.dotY = ScalePx(17, scale);
    m.dotSize = ScalePx(8, scale);

    m.mainFontPx = ScalePx(15, scale);
    m.subFontPx = ScalePx(12, scale);
    m.badgeFontPx = ScalePx(11, scale);

    m.line1 = CardRect{ ScalePx(26, scale), ScalePx(8, scale),
                        m.width - ScalePx(36, scale), ScalePx(21, scale) };
    m.line2 = CardRect{ ScalePx(26, scale), ScalePx(31, scale),
                        m.width - ScalePx(36, scale), ScalePx(20, scale) };
    m.line3 = CardRect{ ScalePx(26, scale), ScalePx(55, scale),
                        m.width - ScalePx(36, scale), ScalePx(16, scale) };

    m.radius = ScalePx(10, scale);
    return m;
}

CardText BuildCardText(const IpInfo& info, bool hasInfo, bool lastError, bool timerWarn,
                       const std::wstring& errMsg, uint64_t updatedAt, uint64_t now)
{
    CardText t;
    t.dot = lastError ? DotState::Warn : (hasInfo ? DotState::Ok : DotState::Idle);

    if (hasInfo) {
        std::wstring place = info.region;
        if (!info.city.empty()) place += (place.empty() ? L"" : L" ") + info.city;

        std::wstring geo = info.country;
        if (!place.empty()) geo += (geo.empty() ? L"" : L" ") + place;

        t.main = geo.empty() ? info.ip : info.ip + L" · " + geo;

        std::wstring tail = ShortAsn(info);
        std::wstring ago = Ago(updatedAt, now);
        if (!tail.empty()) tail += L" · " + ago;
        else tail = ago;

        if (timerWarn) {
            tail += L" · 定时器异常，已退回 1 分钟";
            t.subWarn = true;
        }
        if (lastError) {
            tail += L"（刷新失败，显示上次结果）";
            t.subWarn = true;
        }
        t.sub = tail;
    } else if (lastError) {
        t.main = L"出口 IP 获取失败";
        t.sub = errMsg.empty() ? L"稍后自动重试 · 右键可立即刷新"
                               : errMsg + L" · 右键可立即刷新";
        if (t.sub.size() > 46) t.sub = t.sub.substr(0, 46) + L"…";
        t.subWarn = true;
    } else {
        t.main = L"查询出口 IP 中…";
    }
    return t;
}

void RoundedPath(GraphicsPath& p, float x, float y, float w, float h, float r)
{
    float d = r * 2.0f;
    if (w < d * 1.2f || h < d * 1.2f) {
        p.AddRectangle(RectF(x, y, w, h));
        return;
    }
    p.AddArc(x, y, d, d, 180.0f, 90.0f);
    p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

void DrawCard(Graphics& g, const CardMetrics& m, const CardText& t, bool transparentCorners)
{
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    // ClearType 需要不透明背景；离屏 PNG 用灰度抗锯齿，避免彩边/alpha 脏点
    g.SetTextRenderingHint(transparentCorners ? TextRenderingHintAntiAliasGridFit
                                              : TextRenderingHintClearTypeGridFit);

    if (transparentCorners) {
        g.Clear(Color(0, 0, 0, 0));
        GraphicsPath bg;
        RoundedPath(bg, 0.5f, 0.5f, (REAL)m.width - 1.0f, (REAL)m.height - 1.0f,
                    (REAL)m.radius);
        SolidBrush bgBrush(kBg);
        g.FillPath(&bgBrush, &bg);
    } else {
        g.Clear(kBg);
    }

    Color dot = t.dot == DotState::Bad  ? kBad
              : t.dot == DotState::Warn ? kWarn
              : t.dot == DotState::Ok   ? kDotOk
                                        : kDotIdle;
    SolidBrush dotBrush(dot);
    g.FillEllipse(&dotBrush, (REAL)m.dotX, (REAL)m.dotY, (REAL)m.dotSize, (REAL)m.dotSize);

    auto fontMain = MakeFont((float)m.mainFontPx);
    auto fontSub = MakeFont((float)m.subFontPx);

    StringFormat fmt;
    fmt.SetAlignment(StringAlignmentNear);
    fmt.SetLineAlignment(StringAlignmentCenter);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    fmt.SetFormatFlags(StringFormatFlagsNoWrap);

    SolidBrush textBrush(kText);
    SolidBrush subBrush(t.subWarn ? kWarn : kDim);

    RectF r1((REAL)m.line1.x, (REAL)m.line1.y, (REAL)m.line1.w, (REAL)m.line1.h);
    RectF r2((REAL)m.line2.x, (REAL)m.line2.y, (REAL)m.line2.w, (REAL)m.line2.h);

    g.DrawString(t.main.c_str(), -1, fontMain.get(), r1, &fmt, &textBrush);
    if (!t.sub.empty())
        g.DrawString(t.sub.c_str(), -1, fontSub.get(), r2, &fmt, &subBrush);

    // 第三行：徽章。圆点带颜色，文字统一灰色 —— 颜色是主信号，文字只是兜底
    if (!t.badges.empty()) {
        auto fontBadge = MakeFont((float)m.badgeFontPx);
        StringFormat bf;
        bf.SetAlignment(StringAlignmentNear);
        bf.SetLineAlignment(StringAlignmentCenter);
        bf.SetFormatFlags(StringFormatFlagsNoWrap);

        const REAL y = (REAL)m.line3.y;
        const REAL h = (REAL)m.line3.h;
        const REAL d = (REAL)ScalePx(5, m.scale);
        const REAL gapDot  = (REAL)ScalePx(3, m.scale);
        const REAL gapItem = (REAL)ScalePx(11, m.scale);
        const REAL maxX = (REAL)(m.line3.x + m.line3.w);
        REAL x = (REAL)m.line3.x;

        for (const auto& b : t.badges) {
            RectF measure(0, 0, 1000.0f, h);
            RectF bounds;
            g.MeasureString(b.label.c_str(), -1, fontBadge.get(), measure, &bf, &bounds);
            const REAL w = bounds.Width;
            if (x + d + gapDot + w > maxX) break;   // 放不下就停，不画半截

            if (b.na) {
                // 空心 = 当前配置下这个检查不适用（例如没开隧道时问「隧道接管了吗」）
                // 实心灰 = 测了但没测到。两者绝不能长得一样。
                Pen ring(kDotIdle, 1.0f);
                g.DrawEllipse(&ring, x, y + (h - d) / 2.0f, d, d);
            } else {
                SolidBrush sevBrush(SevColor(b.sev));
                g.FillEllipse(&sevBrush, x, y + (h - d) / 2.0f, d, d);
            }
            x += d + gapDot;

            RectF r(x, y, w + 4.0f, h);
            g.DrawString(b.label.c_str(), -1, fontBadge.get(), r, &bf, &subBrush);
            x += w + gapItem;
        }
    }

    GraphicsPath border;
    RoundedPath(border, 0.5f, 0.5f, (REAL)m.width - 1.0f, (REAL)m.height - 1.0f,
                (REAL)m.radius);
    Pen pen(kBorder, 1.0f);
    g.DrawPath(&pen, &border);
}

bool SavePngToFile(Bitmap& bmp, const std::wstring& path, std::wstring& err)
{
    CLSID png = {};
    if (GetEncoderClsid(L"image/png", &png) < 0) {
        err = L"系统没有 PNG 编码器";
        return false;
    }
    if (bmp.Save(path.c_str(), &png, nullptr) != Ok) {
        err = L"保存 PNG 失败（路径不可写？）";
        return false;
    }
    return true;
}

bool RenderCardToPng(const std::wstring& path, const CardMetrics& m, const CardText& t,
                     std::wstring& err)
{
    Bitmap bmp(m.width, m.height, PixelFormat32bppARGB);
    if (bmp.GetLastStatus() != Ok) {
        err = L"创建位图失败";
        WriteLog(path, L"render FAILED: " + err);
        return false;
    }

    {
        Graphics g(&bmp);
        DrawCard(g, m, t, true);
    }

    if (!SavePngToFile(bmp, path, err)) {
        WriteLog(path, L"render FAILED: " + err);
        return false;
    }

    WriteSidecar(path, m);
    WriteLog(path, L"rendered");
    return true;
}

bool CaptureWindowToPng(HWND hwnd, const std::wstring& path, std::wstring& err)
{
    RECT rc = {};
    if (!GetWindowRect(hwnd, &rc)) {
        err = L"读取窗口位置失败";
        WriteLog(path, L"screenshot FAILED: " + err);
        return false;
    }
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        err = L"窗口尺寸无效";
        WriteLog(path, L"screenshot FAILED: " + err);
        return false;
    }

    HDC screen = GetDC(nullptr);
    if (!screen) {
        err = L"GetDC 失败";
        WriteLog(path, L"screenshot FAILED: " + err);
        return false;
    }
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    if (!mem || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        err = L"创建截图缓冲区失败";
        WriteLog(path, L"screenshot FAILED: " + err);
        return false;
    }

    HGDIOBJ old = SelectObject(mem, bmp);
    BOOL blt = BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);
    SelectObject(mem, old);
    ReleaseDC(nullptr, screen);

    bool ok = false;
    if (!blt) {
        err = L"BitBlt 失败（锁屏/远程会话？）";
    } else {
        Bitmap b(bmp, nullptr);
        CLSID png = {};
        if (b.GetLastStatus() != Ok) {
            err = L"创建 Bitmap 失败";
        } else if (GetEncoderClsid(L"image/png", &png) < 0) {
            err = L"系统没有 PNG 编码器";
        } else if (b.Save(path.c_str(), &png, nullptr) != Ok) {
            err = L"保存 PNG 失败（路径不可写？）";
        } else {
            ok = true;
        }
    }

    DeleteObject(bmp);
    DeleteDC(mem);

    WriteLog(path, ok ? L"screenshot written" : (L"screenshot FAILED: " + err));
    return ok;
}
