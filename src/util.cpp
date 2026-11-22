#include "util.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

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

std::wstring ExeDir()
{
    wchar_t exe[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring(L".");
    std::wstring s(exe, n);
    size_t p = s.find_last_of(L"\\/");
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

    // 默认端口不显示；这里不能用 count=4 去比 ":80"（长度 3，旧代码会越界比较）
    if (host.size() > 4 && host.compare(host.size() - 4, 4, L":443") == 0)
        host.resize(host.size() - 4);
    else if (host.size() > 3 && host.compare(host.size() - 3, 3, L":80") == 0)
        host.resize(host.size() - 3);
    return host;
}
