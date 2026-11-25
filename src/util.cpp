// winsock2.h 必须先于 windows.h；util.h 会带进 windows.h
#include <winsock2.h>
#include <ws2tcpip.h>

#include "util.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>

IpKind ClassifyIp(const std::wstring& text)
{
    // InetPton 不接受首尾空白；配置文件里常带空格，先裁掉
    size_t a = 0, b = text.size();
    while (a < b && iswspace(text[a])) ++a;
    while (b > a && iswspace(text[b - 1])) --b;
    if (a == b) return IpKind::None;
    const std::wstring s = text.substr(a, b - a);

    IN_ADDR v4 = {};
    if (InetPtonW(AF_INET, s.c_str(), &v4) == 1) return IpKind::V4;
    IN6_ADDR v6 = {};
    if (InetPtonW(AF_INET6, s.c_str(), &v6) == 1) return IpKind::V6;
    return IpKind::None;
}

void ParseExpectedText(const std::wstring& text, std::wstring& country, std::wstring& asn)
{
    country.clear();
    asn.clear();
    std::wstring token;

    auto trim = [](const std::wstring& s) {
        size_t a = 0, b = s.size();
        while (a < b && iswspace(s[a])) ++a;
        while (b > a && iswspace(s[b - 1])) --b;
        return s.substr(a, b - a);
    };
    auto flush = [&] {
        const std::wstring t = trim(token);
        token.clear();
        if (t.empty()) return;
        std::wstring up;
        for (wchar_t c : t) up += (wchar_t)towupper(c);
        const bool isAsn = (up[0] >= L'0' && up[0] <= L'9') ||
                           (up.size() >= 2 && up[0] == L'A' && up[1] == L'S');
        if (isAsn) {
            if (asn.empty()) asn = (up[0] >= L'0' && up[0] <= L'9') ? (L"AS" + up) : up;
        } else if (country.empty()) {
            country = up;
        }
    };

    for (wchar_t c : text) {
        if (c == L'/' || c == L',' || c == L';' || iswspace(c)) flush();
        else token += c;
    }
    flush();
}

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& s)
{
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ExePath()
{
    // manifest 声明了 longPathAware；固定 MAX_PATH 的旧写法会把长路径截断。
    DWORD cap = MAX_PATH;
    std::wstring buf(cap, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), cap);
        if (n == 0) return std::wstring();
        if (n < cap) {
            buf.resize(n);
            return buf;
        }
        if (cap >= 32768) return std::wstring();
        cap *= 2;
        buf.assign(cap, L'\0');
    }
}

std::wstring ExeDir()
{
    const std::wstring s = ExePath();
    if (s.empty()) return std::wstring(L".");
    const size_t p = s.find_last_of(L"\\/");
    return (p == std::wstring::npos) ? std::wstring(L".") : s.substr(0, p);
}

uint64_t NowFileTime()
{
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart;
}

std::wstring Ago(uint64_t then, uint64_t now)
{
    if (then == 0) return L"未更新";
    if (now <= then) return L"刚刚";
    uint64_t sec = (now - then) / 10000000ULL;
    if (sec < 10) return L"刚刚";
    if (sec < 60) return std::to_wstring(sec) + L"秒前";
    uint64_t min = sec / 60;
    if (min < 60) return std::to_wstring(min) + L"分钟前";
    uint64_t hour = min / 60;
    if (hour < 24) return std::to_wstring(hour) + L"小时前";
    return std::to_wstring(hour / 24) + L"天前";
}

std::wstring LocalTimeString(uint64_t fileTimeUtc)
{
    if (fileTimeUtc == 0) return std::wstring();
    FILETIME ft;
    ft.dwLowDateTime = (DWORD)(fileTimeUtc & 0xFFFFFFFFULL);
    ft.dwHighDateTime = (DWORD)(fileTimeUtc >> 32);
    SYSTEMTIME utc = {}, local = {};
    if (!FileTimeToSystemTime(&ft, &utc)) return std::wstring();
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) local = utc;
    wchar_t buf[32] = {};
    swprintf(buf, 32, L"%02u:%02u:%02u",
             (unsigned)local.wHour, (unsigned)local.wMinute, (unsigned)local.wSecond);
    return buf;
}

bool FileTimeMinusSeconds(uint64_t base, uint64_t seconds, uint64_t& out)
{
    uint64_t delta = seconds * 10000000ULL;
    if (base < delta) return false;
    out = base - delta;
    return true;
}

int ScalePx(int v, float scale)
{
    return (int)((float)v * scale + 0.5f);
}

float DpiScaleForMonitor(HMONITOR monitor)
{
    float scale = 0.0f;

    typedef HRESULT(WINAPI* GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        GetDpiForMonitorFn fn =
            (GetDpiForMonitorFn)(void*)GetProcAddress(shcore, "GetDpiForMonitor");
        if (fn && monitor) {
            UINT dx = 96, dy = 96;
            if (SUCCEEDED(fn(monitor, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)) && dx != 0)
                scale = (float)dx / 96.0f;
        }
        FreeLibrary(shcore);
    }

    // 老系统没有 shcore：进程本身是 system-DPI aware（manifest/API 降级），
    // 这时系统 DPI 就是正确值。
    if (scale <= 0.0f) {
        HDC dc = GetDC(nullptr);
        int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
        if (dc) ReleaseDC(nullptr, dc);
        if (dpi <= 0) dpi = 96;
        scale = (float)dpi / 96.0f;
    }

    if (scale < 0.5f || scale > 4.0f) scale = 1.0f;
    return scale;
}

std::wstring SourceHost(const std::wstring& url)
{
    size_t p = url.find(L"://");
    if (p == std::wstring::npos) return url;
    p += 3;
    size_t e = url.find_first_of(L"/?#", p);
    std::wstring host = url.substr(p, (e == std::wstring::npos) ? std::wstring::npos : e - p);

    // 不要把 URL 里的 user:token@ 带进菜单/错误文案
    const size_t at = host.rfind(L'@');
    if (at != std::wstring::npos) host = host.substr(at + 1);

    // 默认端口不显示；这里不能用 count=4 去比 ":80"（长度 3，旧代码会越界比较）
    if (host.size() > 4 && host.compare(host.size() - 4, 4, L":443") == 0)
        host.resize(host.size() - 4);
    else if (host.size() > 3 && host.compare(host.size() - 3, 3, L":80") == 0)
        host.resize(host.size() - 3);
    return host;
}
