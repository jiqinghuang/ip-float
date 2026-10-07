// ip-float.cpp — Windows 桌面「出口 IP」实时悬浮小窗
//
// 纯 Win32 + GDI+，MinGW 静态编译成单个 exe，零运行时依赖。
//
// 功能对照 pi 的 extensions/ip-status.ts：
//   · 无边框、置顶、带透明度的圆角小卡片，按住任意处可拖动
//   · 第一行 IP · 国家 地区 城市；第二行 ASN 短名 · 多久前
//   · 定时自动刷新；失败时保留上次结果并把状态点变橙色
//   · 右键菜单：立即刷新 / 刷新间隔 / 设置… / 复制 IP / 总在最前 / 开机自启 / 退出
//   · 托盘图标：显示隐藏（双击）/ 刷新 / 设置 / 退出
//   · 配置写在 %APPDATA%\ip-float\settings.ini
//
// 命令行：
//   ip-float.exe                        正常启动
//   ip-float.exe --demo --shot out.png  假数据渲染一张自检图后退出（不联网）
//   ip-float.exe --demo --shot-dlg d.png  给设置窗口拍一张自检图后退出（不联网）
//   ip-float.exe --source <url>         覆盖数据源（默认 ipinfo.io，备用 ipapi.co）
//   ip-float.exe --interval <分钟>      覆盖刷新间隔
//
// 单实例：已有实例在跑时，第二次启动只会把已有卡片闪一下就退出，不会开出第二张卡片。
// 自检模式（--demo / --shot / --shot-dlg）不受单实例限制，方便随时验证渲染。

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <propidl.h>   // MinGW 的 gdiplus 头依赖 PROPID（Windows SDK 在 propidl/wtypes 里定义）
#include <gdiplus.h>
#include <process.h>
#include <string>
#include <vector>
#include <cstdlib>
#include <cwctype>

using namespace Gdiplus;

// ── 常量 ─────────────────────────────────────────────────────────────────────
static const wchar_t* kWndClass = L"ip-float-window";
static const wchar_t* kDlgClass = L"ip-float-settings";

static const int BASE_W = 340;
static const int BASE_H = 66;

static const UINT ID_TIMER_REFRESH = 1;
static const UINT ID_TIMER_TICK    = 2;
static const UINT ID_TIMER_SHOT    = 3;
static const UINT ID_TIMER_DLGSHOT = 4;   // 设置窗口自检截图
static const UINT ID_TIMER_FLASH    = 5;   // 第二个实例启动时的有限时长闪烁

static const UINT WM_APP_FETCHED  = WM_APP + 1;
static const UINT WM_APP_TRAY     = WM_APP + 2;
static const UINT WM_APP_ACTIVATE = WM_APP + 3;   // 第二个实例让第一个把卡片亮出来

enum {
    IDM_REFRESH = 100,
    IDM_INTERVAL_BASE = 200,
    IDM_SETTINGS = 300,
    IDM_COPY,
    IDM_TOPMOST,
    IDM_AUTOSTART,
    IDM_QUIT,
};

static const int kIntervals[] = { 1, 3, 5, 10, 30, 60 };
static const int kIntervalCount = 6;

static const Color kBg     (255,  30,  31,  34);
static const Color kBorder (255,  60,  65,  75);
static const Color kText   (255, 232, 234, 237);
static const Color kDim    (255, 150, 156, 166);
static const Color kWarn   (255, 224, 175, 104);
static const Color kDotOk  (255, 158, 206, 106);
static const Color kDotIdle(255, 150, 156, 166);

// ── 数据结构 ─────────────────────────────────────────────────────────────────
struct Settings {
    int  intervalMinutes;
    int  opacityPercent;
    bool topMost;
    bool autoStart;
    int  x;
    int  y;

    Settings() : intervalMinutes(5), opacityPercent(94), topMost(true),
                 autoStart(false), x(-1), y(-1) {}
    // x / y == -1 表示「还没摆过」，由 ApplyLayout 放到右上角。
    // 用 -1 而不是 >= 0 判断，是为了允许负坐标（多屏布局里卡片在左侧屏上）。
};

struct IpInfo {
    std::wstring ip, country, region, city, asn, org, source;
    SYSTEMTIME   at;
    IpInfo() { ZeroMemory(&at, sizeof(at)); }
};

// ── 全局状态 ─────────────────────────────────────────────────────────────────
static HWND      g_hwnd  = nullptr;
static HINSTANCE g_inst  = nullptr;
static HICON     g_icon  = nullptr;
static ULONG_PTR g_gdiToken = 0;
static float     g_scale = 1.0f;
static Settings  g_cfg;
static std::wstring g_iniPath;

static bool       g_hasInfo   = false;
static bool       g_lastError = false;
static bool       g_busy      = false;
static IpInfo     g_info;
static SYSTEMTIME g_updatedAt;
static std::wstring g_lastErrMsg;
static std::wstring g_lastErrFull;

static bool         g_demo     = false;
static bool         g_shotDone = false;
static std::wstring g_shotPath;
static std::vector<std::wstring> g_sources;

static NOTIFYICONDATAW g_nid;
static bool g_trayAdded = false;
static const UINT kTrayId = 1;

// 单实例互斥体：进程存活期间一直持有，退出时由系统释放
static const wchar_t* kMutexName = L"Local\\ip-float-single-instance";
static HANDLE    g_mutex = nullptr;

static bool g_fetchPending = false;   // 抓取途中又收到刷新请求，完事后补一次
static bool g_timerWarn    = false;   // SetTimer 失败，已退回最小间隔并提示
static int  g_flashTicks   = 0;       // 闪烁剩余次数（定长，不会一直闪）
static std::wstring g_dlgShotPath;    // --shot-dlg：设置窗口的自检图路径

// ── 小工具 ───────────────────────────────────────────────────────────────────
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}

static int Scale(int v) { return (int)(v * g_scale + 0.5f); }

static std::wstring Ago(const SYSTEMTIME& t)
{
    if (t.wYear == 0) return L"未更新";
    FILETIME f1, f2;
    SystemTimeToFileTime(&t, &f1);
    GetSystemTimeAsFileTime(&f2);
    ULARGE_INTEGER a, b;
    a.LowPart = f1.dwLowDateTime; a.HighPart = f1.dwHighDateTime;
    b.LowPart = f2.dwLowDateTime; b.HighPart = f2.dwHighDateTime;
    if (b.QuadPart <= a.QuadPart) return L"刚刚";
    long sec = (long)((b.QuadPart - a.QuadPart) / 10000000ULL);
    wchar_t buf[64];
    if (sec < 10) return L"刚刚";
    if (sec < 60) { wsprintfW(buf, L"%ld秒前", sec); return buf; }
    long m = sec / 60;
    if (m < 60) { wsprintfW(buf, L"%ld分钟前", m); return buf; }
    long h = m / 60;
    if (h < 24) { wsprintfW(buf, L"%ld小时前", h); return buf; }
    wsprintfW(buf, L"%ld天前", h / 24);
    return buf;
}

static std::wstring ShortAsn(const IpInfo& info)
{
    if (info.asn.empty() && info.org.empty()) return L"";
    struct Alias { const wchar_t* pattern; const wchar_t* name; };
    static const Alias aliases[] = {
        { L"constant|choopa|vultr",       L"Vultr"   },
        { L"digitalocean",                L"DO"      },
        { L"amazon|aws",                  L"AWS"     },
        { L"google",                      L"Google"  },
        { L"microsoft|azure",             L"Azure"   },
        { L"cloudflare",                  L"CF"      },
        { L"oracle",                      L"Oracle"  },
        { L"linode|akamai",               L"Linode"  },
        { L"bandwagon|it7",               L"BWG"     },
        { L"alibaba|aliyun",              L"Aliyun"  },
        { L"tencent",                     L"Tencent" },
        { L"china telecom|chinatelecom",  L"CT"      },
        { L"china unicom|chinaunicom",    L"CU"      },
    };
    std::wstring org = info.org;
    std::wstring lower = org;
    for (size_t i = 0; i < lower.size(); ++i) lower[i] = towlower(lower[i]);
    std::wstring shown = org;
    for (int i = 0; i < (int)(sizeof(aliases) / sizeof(aliases[0])); ++i) {
        const wchar_t* p = aliases[i].pattern;
        std::wstring key(p);
        // 逐个候选词做子串匹配，避免引入正则
        size_t start = 0;
        while (start <= key.size()) {
            size_t bar = key.find(L'|', start);
            std::wstring word = key.substr(start, (bar == std::wstring::npos ? key.size() : bar) - start);
            if (!word.empty() && lower.find(word) != std::wstring::npos) {
                shown = aliases[i].name;
                break;
            }
            if (bar == std::wstring::npos) break;
            start = bar + 1;
        }
        if (shown != org) break;
    }
    if (shown == org && shown.size() > 14) {
        size_t comma = shown.find(L',');
        if (comma != std::wstring::npos) shown = shown.substr(0, comma);
        const wchar_t* tails[] = { L"LLC", L"Inc", L"Corp", L"Corporation", L"Ltd", L"Limited", L"LLP" };
        for (int i = 0; i < 7; ++i) {
            std::wstring tail = std::wstring(L" ") + tails[i];
            if (shown.size() > tail.size() &&
                _wcsicmp(shown.c_str() + shown.size() - tail.size(), tail.c_str()) == 0) {
                shown = shown.substr(0, shown.size() - tail.size());
            }
        }
        while (!shown.empty() && (shown[shown.size() - 1] == L' ' || shown[shown.size() - 1] == L'.'))
            shown.erase(shown.size() - 1);
        // 取前两个词
        size_t sp1 = shown.find(L' ');
        if (sp1 != std::wstring::npos) {
            size_t sp2 = shown.find(L' ', sp1 + 1);
            if (sp2 != std::wstring::npos) shown = shown.substr(0, sp2);
        }
        if (shown.size() > 14) shown = shown.substr(0, 14);
    }
    if (!info.asn.empty())
        return shown.empty() ? info.asn : info.asn + L" " + shown;
    return shown;
}

// ── 配置（INI） ──────────────────────────────────────────────────────────────
static std::wstring ExeDir()
{
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring s = exe;
    size_t p = s.find_last_of(L'\\');
    return (p == std::wstring::npos) ? std::wstring(L".") : s.substr(0, p);
}

static bool DirWritable(const std::wstring& dir)
{
    std::wstring probe = dir + L"\\.ip-float-write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// 拷 ini：降级到便携模式时把 APPDATA 里的老配置带过来
static bool CopyIni(const std::wstring& from, const std::wstring& to)
{
    HANDLE s = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (s == INVALID_HANDLE_VALUE) return false;
    HANDLE d = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (d == INVALID_HANDLE_VALUE) { CloseHandle(s); return false; }
    char buf[8192];
    DWORD n = 0;
    bool ok = true;
    while (ReadFile(s, buf, sizeof(buf), &n, nullptr) && n) {
        DWORD wr = 0;
        if (!WriteFile(d, buf, n, &wr, nullptr)) { ok = false; break; }
    }
    CloseHandle(s);
    CloseHandle(d);
    return ok;
}

// 优先 %APPDATA%\ip-float\settings.ini；写不进去就退到 exe 同目录（便携用法）
static std::wstring IniPath()
{
    if (!g_iniPath.empty()) return g_iniPath;
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring portable = ExeDir() + L"\\ip-float.ini";
    if (n > 0 && n < MAX_PATH) {
        std::wstring dir = std::wstring(buf) + L"\\ip-float";
        CreateDirectoryW(dir.c_str(), nullptr);
        if (DirWritable(dir)) {
            g_iniPath = dir + L"\\settings.ini";
            return g_iniPath;
        }
        // 降级了：APPDATA 里已经有配置就搬过来，否则用户会“丢设置”而不自知。
        // 已经有便携配置则不动（不覆盖）。
        std::wstring old = dir + L"\\settings.ini";
        if (GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(portable.c_str()) == INVALID_FILE_ATTRIBUTES)
            CopyIni(old, portable);
    }
    g_iniPath = portable;
    return g_iniPath;
}

static void LoadSettings(Settings& s)
{
    g_iniPath = IniPath();
    const wchar_t* p = g_iniPath.c_str();
    s.intervalMinutes = (int)GetPrivateProfileIntW(L"ip-float", L"intervalMinutes", 5, p);
    s.opacityPercent  = (int)GetPrivateProfileIntW(L"ip-float", L"opacityPercent", 94, p);
    s.topMost         = GetPrivateProfileIntW(L"ip-float", L"topMost", 1, p) != 0;
    s.autoStart       = GetPrivateProfileIntW(L"ip-float", L"autoStart", 0, p) != 0;
    s.x               = (int)GetPrivateProfileIntW(L"ip-float", L"x", -1, p);
    s.y               = (int)GetPrivateProfileIntW(L"ip-float", L"y", -1, p);
    if (s.intervalMinutes < 1) s.intervalMinutes = 1;
    if (s.intervalMinutes > 1440) s.intervalMinutes = 1440;
    if (s.opacityPercent < 0) s.opacityPercent = 0;    // 0 = 完全透明（靠托盘图标找回来）
    if (s.opacityPercent > 100) s.opacityPercent = 100;
}

static void SaveSettings()
{
    if (g_iniPath.empty()) g_iniPath = IniPath();
    const wchar_t* p = g_iniPath.c_str();
    wchar_t buf[32];
    wsprintfW(buf, L"%d", g_cfg.intervalMinutes); WritePrivateProfileStringW(L"ip-float", L"intervalMinutes", buf, p);
    wsprintfW(buf, L"%d", g_cfg.opacityPercent);  WritePrivateProfileStringW(L"ip-float", L"opacityPercent", buf, p);
    WritePrivateProfileStringW(L"ip-float", L"topMost",   g_cfg.topMost   ? L"1" : L"0", p);
    WritePrivateProfileStringW(L"ip-float", L"autoStart", g_cfg.autoStart ? L"1" : L"0", p);
    wsprintfW(buf, L"%d", g_cfg.x); WritePrivateProfileStringW(L"ip-float", L"x", buf, p);
    wsprintfW(buf, L"%d", g_cfg.y); WritePrivateProfileStringW(L"ip-float", L"y", buf, p);
}

// 注册表是「开机自启」的唯一真相：ini 只是缓存，手工删掉 Run 项后下次启动就会同步回来
static bool AutoStartEnabled()
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    wchar_t val[MAX_PATH * 2] = {};
    DWORD cb = sizeof(val);
    bool on = RegQueryValueExW(k, L"ip-float", nullptr, nullptr, (LPBYTE)val, &cb) == ERROR_SUCCESS && val[0] != 0;
    RegCloseKey(k);
    return on;
}

static void SetAutoStart(bool on)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring quoted = std::wstring(L"\"") + exe + L"\"";
        RegSetValueExW(k, L"ip-float", 0, REG_SZ, (const BYTE*)quoted.c_str(),
                       (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"ip-float");
    }
    RegCloseKey(k);
}

// ── HTTP + JSON ──────────────────────────────────────────────────────────────
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

static std::wstring WinHttpErrorText(DWORD code)
{
    const wchar_t* hint = nullptr;
    switch (code) {
    case 12002: hint = L"超时"; break;
    case 12007: hint = L"域名解析失败（DNS 或网络不通）"; break;
    case 12029: hint = L"无法连接（端口不通、被墙，或该走代理）"; break;
    case 12030: hint = L"连接被中断"; break;
    case 12031: hint = L"连接被重置"; break;
    case 12175: hint = L"TLS 握手失败"; break;
    case 12182: hint = L"证书颁发机构不受信任"; break;
    case 12183: hint = L"证书域名不匹配"; break;
    case 12184: hint = L"证书已过期"; break;
    case 12185: hint = L"证书用途不匹配（常见于代理/中间人拦截）"; break;
    case 12186: hint = L"证书无效"; break;
    case 12187: hint = L"证书吊销状态无法确认"; break;
    default: break;
    }
    wchar_t buf[160];
    if (hint) wsprintfW(buf, L"%u：%s", (unsigned)code, hint);
    else      wsprintfW(buf, L"%u", (unsigned)code);
    return buf;
}

// HTTP 状态码翻成人话。ipinfo.io 未注册 token时限流很凶，429 必须说清楚，
// 否则用户只会看到干巴巴一个数字，不知道该等还是该换源。
static std::wstring HttpStatusText(DWORD status)
{
    switch (status) {
    case 401: return L"401：数据源要求认证（ipinfo 需要带 token，换 --source 或用 ipapi.co）";
    case 403: return L"403：被拒绝（数据源要 token，或你的出口 IP 被限流封禁）";
    case 404: return L"404：地址不存在（--source 写错了？）";
    case 429: return L"429：请求太频繁，被数据源限流（调大间隔，或用 --source 换源）";
    case 500: return L"500：数据源自己出错了，稍后自动重试";
    case 503: return L"503：数据源暂时不可用，稍后自动重试";
    default: break;
    }
    wchar_t buf[64];
    wsprintfW(buf, L"HTTP %u", (unsigned)status);
    return buf;
}

static std::wstring CodeText(const wchar_t* what, DWORD code)
{
    return std::wstring(what) + L" " + WinHttpErrorText(code);
}

static bool HttpGet(const std::wstring& url, std::string& body, std::wstring& err)
{
    URL_COMPONENTS uc;
    ZeroMemory(&uc, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    uc.lpszHostName = host;   uc.dwHostNameLength = 255;
    uc.lpszUrlPath  = path;   uc.dwUrlPathLength  = 2047;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { err = L"URL 解析失败"; return false; }

    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    HINTERNET session = WinHttpOpen(L"ip-float/3.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { err = CodeText(L"WinHttpOpen 失败", GetLastError()); return false; }
    WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);

    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (!conn) { err = CodeText(L"连接失败", GetLastError()); WinHttpCloseHandle(session); return false; }

    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { err = CodeText(L"请求创建失败", GetLastError()); WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return false; }

    WinHttpAddRequestHeaders(req, L"Accept: application/json", (ULONG)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != 0;
    DWORD failCode = ok ? 0 : GetLastError();
    if (ok) {
        ok = WinHttpReceiveResponse(req, nullptr) != 0;
        if (!ok) failCode = GetLastError();
    }

    DWORD status = 0, len = sizeof(status);
    if (ok)
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

    if (ok && status == 200) {
        body.clear();
        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &read) || read == 0) break;
            chunk.resize(read);
            body += chunk;
            if (body.size() > 1u << 20) break;   // 1 MiB 上限，防跑飞
        } while (avail > 0);
        ok = !body.empty();
        if (!ok) err = L"响应为空";
    } else {
        err = ok ? HttpStatusText(status) : CodeText(L"请求失败", failCode);
        ok = false;
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return ok;
}

// 读 \uXXXX（i 指向反斜杠）→ 码点；不是合法四位十六进制就返回 false
static bool JsonHex4(const std::string& json, size_t i, unsigned& out)
{
    out = 0;
    for (int k = 2; k < 6; ++k) {
        char h = json[i + k];
        unsigned d;
        if (h >= '0' && h <= '9')      d = h - '0';
        else if (h >= 'a' && h <= 'f') d = h - 'a' + 10;
        else if (h >= 'A' && h <= 'F') d = h - 'A' + 10;
        else return false;
        out = out * 16 + d;
    }
    return true;
}

// 码点 → UTF-8；正确处理 BMP 之外的四字节序列
static void AppendUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string JsonStr(const std::string& json, const std::string& key)
{
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    while (pos != std::string::npos) {
        size_t i = pos + pat.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (i < json.size() && json[i] == ':') {
            ++i;
            while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
            if (i < json.size() && json[i] == '"') {
                ++i;
                std::string out;
                while (i < json.size() && json[i] != '"') {
                    if (json[i] == '\\' && i + 1 < json.size()) {
                        char c = json[i + 1];
                        if (c == 'n') out += '\n';
                        else if (c == 't') out += '\t';
                        else if (c == 'u' && i + 5 < json.size()) {
                            unsigned code = 0;
                            if (!JsonHex4(json, i, code)) {
                                out += json[i];              // 不是合法 \uXXXX，原样输出
                            } else {
                                i += 5;                       // i 停在最后一个十六进制位
                                // 高代理 + 低代理 → 合成 BMP 之外的码点（emoji、生僻字）
                                if (code >= 0xD800u && code <= 0xDBFFu &&
                                    i + 6 < json.size() &&
                                    json[i + 1] == '\\' && json[i + 2] == 'u') {
                                    unsigned lo = 0;
                                    if (JsonHex4(json, i + 1, lo) &&
                                        lo >= 0xDC00u && lo <= 0xDFFFu) {
                                        code = 0x10000u + ((code - 0xD800u) << 10)
                                             + (lo - 0xDC00u);
                                        i += 6;
                                    }
                                }
                                // 落单的代理项还原不了，按 Unicode 惯例写 U+FFFD
                                if (code >= 0xD800u && code <= 0xDFFFu) code = 0xFFFDu;
                                AppendUtf8(out, code);
                            }
                        } else {
                            out += c;
                            ++i;
                        }
                        ++i;
                    } else {
                        out += json[i];
                        ++i;
                    }
                }
                return out;
            }
            return "";
        }
        pos = json.find(pat, pos + 1);
    }
    return "";
}

static bool ParseIpInfo(const std::string& json, const std::wstring& source, IpInfo& out, std::wstring& err)
{
    out = IpInfo();
    out.ip = Utf8ToWide(JsonStr(json, "ip"));
    if (out.ip.empty()) { err = L"响应里没有 ip 字段"; return false; }

    std::string country = JsonStr(json, "country_code");
    if (country.empty()) country = JsonStr(json, "country");
    out.country = Utf8ToWide(country);
    out.region  = Utf8ToWide(JsonStr(json, "region"));
    out.city    = Utf8ToWide(JsonStr(json, "city"));
    std::wstring org = Utf8ToWide(JsonStr(json, "org"));
    out.asn = Utf8ToWide(JsonStr(json, "asn"));

    // ipinfo.io 把 ASN 塞在 org 里："AS20473 The Constant Company, LLC"
    if (org.size() > 2 && (org[0] == L'A' || org[0] == L'a') && (org[1] == L'S' || org[1] == L's')) {
        size_t sp = org.find(L' ');
        std::wstring head = org.substr(0, sp == std::wstring::npos ? org.size() : sp);
        bool digits = head.size() > 2;
        for (size_t i = 2; i < head.size() && digits; ++i)
            if (head[i] < L'0' || head[i] > L'9') digits = false;
        if (digits) {
            out.asn = head;
            for (size_t i = 0; i < out.asn.size(); ++i) out.asn[i] = towupper(out.asn[i]);
            out.org = (sp == std::wstring::npos) ? L"" : org.substr(sp + 1);
            while (!out.org.empty() && out.org[0] == L' ') out.org.erase(0, 1);
        } else {
            out.org = org;
        }
    } else {
        out.org = org;
    }
    if (!out.org.empty() && out.org == org && !out.asn.empty() && out.asn.size() < 3)
        out.asn.clear();
    if (!out.asn.empty() && out.asn[0] != L'A') out.asn = L"AS" + out.asn;

    out.source = source;
    // 存 UTC：SystemTimeToFileTime / Ago() 都按 UTC 处理，混用本地时间会算错时差
    GetSystemTime(&out.at);
    return true;
}

// 取数据：主源失败自动换备源。在工作线程里跑。
// err 是给卡片看的简短原因，errFull 是给右键菜单看的「URL → 原因」。
static bool FetchNow(IpInfo& out, std::wstring& err, std::wstring& errFull)
{
    std::wstring lastErr = L"没有可用数据源";
    std::wstring lastFull = lastErr;
    for (size_t i = 0; i < g_sources.size(); ++i) {
        std::string body;
        std::wstring e;
        if (HttpGet(g_sources[i], body, e)) {
            if (ParseIpInfo(body, g_sources[i], out, e)) return true;
        }
        lastErr = e;
        lastFull = g_sources[i] + L" → " + e;
    }
    err = lastErr;
    errFull = lastFull;
    return false;
}

// ── 设置窗口 ─────────────────────────────────────────────────────────────────
// 布局全部由字体度量推导（不硬编码像素），尺寸统一经 Scale() 适配 DPI；
// 文案用 GDI 绘制、与测量共用同一套 HFONT，因此永远不会被截断或压行；
// 配色与主卡片一致（深色），标题栏在 Win11 上跟随深色。
static void RoundedPath(GraphicsPath& p, float x, float y, float w, float h, float r);
static void TakeShot(HWND hwnd, const std::wstring& path);

static const Color kDlgFill  (255,  38,  40,  45);
static const Color kFieldBg  (255,  50,  53,  60);
static const Color kAccent   (255,  82, 139, 255);
static const Color kAccentLo (255,  56,  92, 168);

// 一行「标签 + 圆角输入区 + 单位 + 区间提示」，所有 rect 同源计算
struct DlgField {
    RECT label = {};
    RECT box   = {};
    RECT edit  = {};
    RECT unit  = {};
    RECT hint  = {};
};

struct SettingsDlg {
    HWND  hwnd      = nullptr;
    HWND  edit      = nullptr;
    HWND  editOpacity = nullptr;
    HWND  chkTop    = nullptr;
    HWND  chkAuto   = nullptr;
    HWND  btnOk     = nullptr;
    HWND  btnCancel = nullptr;

    bool  done      = false;
    bool  ok        = false;
    int   interval  = 5;
    int   opacity   = 94;
    bool  topMost   = true;
    bool  autoStart = false;
    int   focus     = 0;          // 0 = 刷新间隔获得焦点，1 = 不透明度

    // 布局（像素，已按 DPI 缩放）
    DlgField fInterval = {};
    DlgField fOpacity  = {};
    RECT  noteRc     = {};
    RECT  chkTopRc   = {};
    RECT  chkAutoRc  = {};
    RECT  btnOkRc    = {};
    RECT  btnCancelRc= {};
};
static SettingsDlg g_dlg;

struct DlgMetrics {
    HFONT  base  = nullptr;
    HFONT  small = nullptr;
    HBRUSH fieldBrush = nullptr;
    HBRUSH bgBrush    = nullptr;   // 对话框底色（BS_OWNERDRAW 控件会自己擦成系统浅灰，得重铺）
    int    baseH = 0, smallH = 0;
    int    pad = 0, gap = 0, box = 0;
    int    fieldH = 0, checkH = 0, btnH = 0, btnW = 0;
    int    editW = 0, inset = 0;
    int    clientW = 0, clientH = 0;
};
static DlgMetrics g_dm;

static HFONT DlgFont(int px)
{
    return CreateFontW(-px, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
}

// 度量与绘制共用同一 HFONT，保证「量出来的宽度」就是「画出来的宽度」
static int DlgTextW(HDC dc, HFONT f, const wchar_t* s)
{
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    SelectObject(dc, old);
    return sz.cx;
}

static int DlgLineH(HDC dc, HFONT f)
{
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    return tm.tmHeight;
}

static void DlgTextIn(HDC dc, HFONT f, COLORREF col, const RECT& rc,
                      const wchar_t* s, UINT flags)
{
    HGDIOBJ old = SelectObject(dc, f);
    SetTextColor(dc, col);
    RECT r = rc;
    DrawTextW(dc, s, -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

// 一次算清所有尺寸与位置：clientW/clientH 与每个控件的 rect 同源，不会互相打架
static void DlgBuildMetrics(HDC dc)
{
    g_dm = DlgMetrics();
    g_dm.base  = DlgFont(Scale(15));
    g_dm.small = DlgFont(Scale(13));
    g_dm.baseH  = DlgLineH(dc, g_dm.base);
    g_dm.smallH = DlgLineH(dc, g_dm.small);
    g_dm.pad    = Scale(24);
    g_dm.gap    = Scale(12);
    g_dm.box    = Scale(18);
    g_dm.fieldH = g_dm.baseH + Scale(18);
    g_dm.checkH = g_dm.baseH + Scale(10);
    g_dm.btnH   = g_dm.baseH + Scale(16);
    g_dm.btnW   = Scale(94);
    g_dm.editW  = Scale(64);
    g_dm.inset  = Scale(14);
    g_dm.fieldBrush = CreateSolidBrush(RGB(50, 53, 60));
    g_dm.bgBrush    = CreateSolidBrush(RGB(38, 40, 45));

    // 宽度取各候选里最宽的一条，文案再长也不会被截断
    int boxText = g_dm.box + Scale(12);
    int w = g_dm.pad * 2 + Scale(300);
    int c;
    c = g_dm.pad * 2 + boxText + DlgTextW(dc, g_dm.base,  L"开机时自动启动");
    if (c > w) w = c;
    c = g_dm.pad * 2 + g_dm.btnW * 2 + g_dm.gap;
    if (c > w) w = c;
    c = g_dm.pad * 2 + boxText + DlgTextW(dc, g_dm.small, L"登录 Windows 时自动运行");
    if (c > w) w = c;
    // 「不透明度」标签也不能被挤掉
    c = g_dm.pad * 2 + DlgTextW(dc, g_dm.base, L"不透明度");
    if (c > w) w = c;
    g_dm.clientW = w;

    const int pad = g_dm.pad;
    const int cw  = g_dm.clientW;
    int y = pad;

    // 一个输入区：上方标签，下面是圆角输入区 + 右侧区间提示
    auto field = [&](DlgField& f, const wchar_t* hint) {
        f.label = RECT{ pad, y, cw - pad, y + g_dm.baseH };
        y += g_dm.baseH + Scale(8);
        f.box   = RECT{ pad, y, cw - pad, y + g_dm.fieldH };
        y += g_dm.fieldH + Scale(18);

        const int editH = g_dm.baseH + Scale(6);
        const int et = f.box.top + (g_dm.fieldH - editH) / 2;
        f.edit  = RECT{ f.box.left + g_dm.inset, et,
                        f.box.left + g_dm.inset + g_dm.editW, et + editH };
        f.unit  = RECT{ f.edit.right + Scale(10),
                        f.box.top + (g_dm.fieldH - g_dm.baseH) / 2,
                        f.edit.right + Scale(10) + Scale(120),
                        f.box.top + (g_dm.fieldH - g_dm.baseH) / 2 + g_dm.baseH };
        // 提示文字右对齐：宽度先量出来，起点就不会撞上单位文字
        const int hw = DlgTextW(dc, g_dm.small, hint);
        f.hint = RECT{ f.box.right - g_dm.inset - hw,
                        f.box.top + (g_dm.fieldH - g_dm.smallH) / 2,
                        f.box.right - g_dm.inset,
                        f.box.top + (g_dm.fieldH - g_dm.smallH) / 2 + g_dm.smallH };
    };

    field(g_dlg.fInterval, L"1 - 1440");
    field(g_dlg.fOpacity,  L"0 - 100");

    g_dlg.chkTopRc  = { pad, y, cw - pad, y + g_dm.checkH };
    y += g_dm.checkH + g_dm.gap;

    g_dlg.chkAutoRc = { pad, y, cw - pad, y + g_dm.checkH };
    y += g_dm.checkH + Scale(4);

    g_dlg.noteRc    = { pad + boxText, y, cw - pad, y + g_dm.smallH };
    y += g_dm.smallH + Scale(22);

    g_dlg.btnOkRc     = { cw - pad - g_dm.btnW, y, cw - pad, y + g_dm.btnH };
    g_dlg.btnCancelRc = { cw - pad - g_dm.btnW * 2 - g_dm.gap, y,
                          cw - pad - g_dm.btnW - g_dm.gap, y + g_dm.btnH };
    y += g_dm.btnH + pad;
    g_dm.clientH = y;
}

static void DlgCreateChildren(HWND h)
{
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                    const RECT& r, int id) {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                               r.left, r.top, r.right - r.left, r.bottom - r.top,
                               h, (HMENU)(INT_PTR)id, g_inst, nullptr);
    };

    wchar_t buf[16];
    wsprintfW(buf, L"%d", g_dlg.interval);

    g_dlg.edit = make(L"EDIT", buf, ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER,
                      g_dlg.fInterval.edit, 1001);
    SendMessageW(g_dlg.edit, WM_SETFONT, (WPARAM)g_dm.base, TRUE);

    wsprintfW(buf, L"%d", g_dlg.opacity);
    g_dlg.editOpacity = make(L"EDIT", buf, ES_NUMBER | ES_AUTOHSCROLL | ES_CENTER,
                             g_dlg.fOpacity.edit, 1004);
    SendMessageW(g_dlg.editOpacity, WM_SETFONT, (WPARAM)g_dm.base, TRUE);

    g_dlg.chkTop    = make(L"BUTTON", L"窗口总在最前", BS_OWNERDRAW, g_dlg.chkTopRc,   1002);
    g_dlg.chkAuto   = make(L"BUTTON", L"开机时自动启动", BS_OWNERDRAW, g_dlg.chkAutoRc, 1003);
    g_dlg.btnOk     = make(L"BUTTON", L"确定", BS_OWNERDRAW, g_dlg.btnOkRc,     IDOK);
    g_dlg.btnCancel = make(L"BUTTON", L"取消", BS_OWNERDRAW, g_dlg.btnCancelRc, IDCANCEL);

    // 打开即聚焦输入框
    SetFocus(g_dlg.edit);
}

// 读输入框里的整数并夹到 [lo, hi]；空输入框按“保持原值”处理——
// 否则不小心清空 + 确定，会把卡片直接变成 0% 透明然后“消失”。
static int ReadIntField(HWND edit, int fallback, int lo, int hi)
{
    wchar_t buf[32] = {};
    GetWindowTextW(edit, buf, 32);
    if (buf[0] == L'\0') return fallback;
    int v = _wtoi(buf);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static LRESULT CALLBACK DlgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        DlgCreateChildren(h);
        return 0;

    case WM_ERASEBKGND:
        return 1;                       // 统一在 WM_PAINT 里整块重绘，避免闪烁

    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, RGB(232, 234, 237));
        SetBkColor(dc, RGB(50, 53, 60));
        return (LRESULT)g_dm.fieldBrush;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc = {};
        GetClientRect(h, &rc);

        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        HBRUSH bg = CreateSolidBrush(RGB(38, 40, 45));
        FillRect(mem, &rc, bg);
        DeleteObject(bg);

        // 输入区：圆角底 + 边框（聚焦时描边变强调色）
        auto drawField = [&](const DlgField& f, bool focused) {
            Graphics g(mem);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            REAL fw = (REAL)(f.box.right - f.box.left);
            REAL fh = (REAL)(f.box.bottom - f.box.top);
            GraphicsPath p;
            RoundedPath(p, (REAL)f.box.left + 0.5f, (REAL)f.box.top + 0.5f,
                        fw - 1.0f, fh - 1.0f, (REAL)Scale(9));
            SolidBrush fill(kFieldBg);
            g.FillPath(&fill, &p);
            Pen pen(focused ? kAccent : kBorder, 1.0f);
            g.DrawPath(&pen, &p);
        };
        drawField(g_dlg.fInterval, g_dlg.focus == 0);
        drawField(g_dlg.fOpacity,  g_dlg.focus == 1);

        SetBkMode(mem, TRANSPARENT);
        DlgTextIn(mem, g_dm.base, RGB(232, 234, 237), g_dlg.fInterval.label, L"刷新间隔", DT_LEFT);
        DlgTextIn(mem, g_dm.base, RGB(232, 234, 237), g_dlg.fOpacity.label,  L"不透明度", DT_LEFT);
        DlgTextIn(mem, g_dm.base, RGB(150, 156, 166), g_dlg.fInterval.unit, L"分钟", DT_LEFT);
        DlgTextIn(mem, g_dm.base, RGB(150, 156, 166), g_dlg.fOpacity.unit,  L"%",    DT_LEFT);
        DlgTextIn(mem, g_dm.small, RGB(150, 156, 166), g_dlg.fInterval.hint, L"1 - 1440", DT_RIGHT);
        DlgTextIn(mem, g_dm.small, RGB(150, 156, 166), g_dlg.fOpacity.hint,  L"0 - 100",   DT_RIGHT);
        DlgTextIn(mem, g_dm.small, RGB(118, 124, 134), g_dlg.noteRc,
                  L"登录 Windows 时自动运行", DT_LEFT);

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lp;
        HDC dc = dis->hDC;
        RECT r = dis->rcItem;
        bool pressed = (dis->itemState & ODS_SELECTED) != 0;

        // BS_OWNERDRAW 的 BUTTON 窗口会先用系统浅灰把自己擦一遍，
        // 不重铺底色的话，整行复选框就是一块浅灰方块。
        FillRect(dc, &r, g_dm.bgBrush);

        if (dis->CtlID == 1002 || dis->CtlID == 1003) {
            // ── 自绘复选框
            bool on  = (dis->CtlID == 1002) ? g_dlg.topMost : g_dlg.autoStart;
            int  box = g_dm.box;
            int  top = r.top + (r.bottom - r.top - box) / 2;
            {
                Graphics g(dc);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                GraphicsPath p;
                RoundedPath(p, (REAL)r.left + 0.5f, (REAL)top + 0.5f,
                            (REAL)box - 1.0f, (REAL)box - 1.0f, (REAL)Scale(5));
                if (on) {
                    SolidBrush b(pressed ? kAccentLo : kAccent);
                    g.FillPath(&b, &p);
                    Pen pen(Color(255, 255, 255, 255), (REAL)Scale(2));
                    pen.SetStartCap(LineCapRound);
                    pen.SetEndCap(LineCapRound);
                    REAL bx = (REAL)r.left, by = (REAL)top, bs = (REAL)box;
                    g.DrawLine(&pen, bx + bs * 0.26f, by + bs * 0.54f, bx + bs * 0.45f, by + bs * 0.72f);
                    g.DrawLine(&pen, bx + bs * 0.45f, by + bs * 0.72f, bx + bs * 0.76f, by + bs * 0.28f);
                } else {
                    SolidBrush b(kFieldBg);
                    g.FillPath(&b, &p);
                    Pen pen(pressed ? kAccent : kBorder, 1.0f);
                    g.DrawPath(&pen, &p);
                }
                if (dis->itemState & ODS_FOCUS) {
                    GraphicsPath fp;
                    RoundedPath(fp, (REAL)r.left - 2.5f, (REAL)top - 2.5f,
                                (REAL)box + 5.0f, (REAL)box + 5.0f, (REAL)Scale(7));
                    Pen fpen(Color(110, 82, 139, 255), 1.0f);
                    g.DrawPath(&fpen, &fp);
                }
            }
            wchar_t text[128] = {};
            GetWindowTextW(dis->hwndItem, text, 128);
            RECT tr = { r.left + box + Scale(12), r.top, r.right, r.bottom };
            SetBkMode(dc, TRANSPARENT);
            DlgTextIn(dc, g_dm.base, RGB(232, 234, 237), tr, text, DT_LEFT | DT_VCENTER);
        } else {
            // ── 自绘按钮：确定为主按钮（实心强调色），取消为次级（描边）
            bool primary = (dis->CtlID == IDOK);
            {
                Graphics g(dc);
                g.SetSmoothingMode(SmoothingModeAntiAlias);
                GraphicsPath p;
                RoundedPath(p, (REAL)r.left + 0.5f, (REAL)r.top + 0.5f,
                            (REAL)(r.right - r.left) - 1.0f, (REAL)(r.bottom - r.top) - 1.0f,
                            (REAL)Scale(7));
                if (primary) {
                    SolidBrush b(pressed ? kAccentLo : kAccent);
                    g.FillPath(&b, &p);
                } else {
                    SolidBrush b(kFieldBg);
                    g.FillPath(&b, &p);
                    Pen pen(pressed ? kAccent : kBorder, 1.0f);
                    g.DrawPath(&pen, &p);
                }
            }
            wchar_t text[64] = {};
            GetWindowTextW(dis->hwndItem, text, 64);
            SetBkMode(dc, TRANSPARENT);
            DlgTextIn(dc, g_dm.base, primary ? RGB(255, 255, 255) : RGB(232, 234, 237),
                      r, text, DT_CENTER | DT_VCENTER);
        }
        return TRUE;
    }

    case WM_COMMAND: {
        int  id   = LOWORD(wp);
        UINT code = HIWORD(wp);

        if ((id == 1001 || id == 1004) && (code == EN_SETFOCUS || code == EN_KILLFOCUS)) {
            int which = (id == 1004) ? 1 : 0;
            if (code == EN_SETFOCUS) g_dlg.focus = which;
            InvalidateRect(h, which ? &g_dlg.fOpacity.box : &g_dlg.fInterval.box, FALSE);
            return 0;
        }
        if (id == IDOK) {
            g_dlg.interval = ReadIntField(g_dlg.edit,        g_cfg.intervalMinutes, 1, 1440);
            g_dlg.opacity  = ReadIntField(g_dlg.editOpacity, g_cfg.opacityPercent,  0, 100);

            g_dlg.ok       = true;
            g_dlg.done     = true;
            DestroyWindow(h);
        } else if (id == IDCANCEL) {
            g_dlg.done = true;
            DestroyWindow(h);
        } else if (id == 1002) {
            g_dlg.topMost = !g_dlg.topMost;
            InvalidateRect(g_dlg.chkTop, nullptr, TRUE);
        } else if (id == 1003) {
            g_dlg.autoStart = !g_dlg.autoStart;
            InvalidateRect(g_dlg.chkAuto, nullptr, TRUE);
        }
        return 0;
    }

    case WM_TIMER:
        // --shot-dlg：等界面画完再拍一张，然后自己关掉
        if (wp == ID_TIMER_DLGSHOT) {
            KillTimer(h, ID_TIMER_DLGSHOT);
            TakeShot(h, g_dlgShotPath);
            g_dlg.done = true;
            DestroyWindow(h);
            return 0;
        }
        break;

    case WM_CLOSE:
        g_dlg.done = true;
        DestroyWindow(h);
        return 0;

    case WM_DESTROY:
        g_dlg.hwnd = nullptr;
        g_dlg.done = true;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// 返回 true 表示用户点了确定
static bool ShowSettings(HWND parent)
{
    g_dlg = SettingsDlg();
    g_dlg.interval  = g_cfg.intervalMinutes;
    g_dlg.opacity   = g_cfg.opacityPercent;
    g_dlg.topMost   = g_cfg.topMost;
    g_dlg.autoStart = g_cfg.autoStart;

    HDC sdc = GetDC(nullptr);
    DlgBuildMetrics(sdc);
    ReleaseDC(nullptr, sdc);

    DWORD style   = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_TOPMOST;
    RECT wr = { 0, 0, g_dm.clientW, g_dm.clientH };
    AdjustWindowRectEx(&wr, style, FALSE, exStyle);
    int winW = wr.right - wr.left;
    int winH = wr.bottom - wr.top;

    RECT pr = {};
    GetWindowRect(parent, &pr);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    GetMonitorInfoW(MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST), &mi);

    int x = pr.left - winW - Scale(14);                       // 默认贴在卡片左边
    if (x < mi.rcWork.left) x = pr.right + Scale(14);         // 左边放不下就放右边
    if (x + winW > mi.rcWork.right) x = mi.rcWork.right - winW;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    int y = pr.top;
    if (y + winH > mi.rcWork.bottom) y = mi.rcWork.bottom - winH;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    HWND dlg = CreateWindowExW(exStyle, kDlgClass, L"ip-float 设置", style,
                               x, y, winW, winH, parent, nullptr, g_inst, nullptr);
    if (!dlg) {
        if (g_dm.fieldBrush) DeleteObject(g_dm.fieldBrush);
        if (g_dm.bgBrush)    DeleteObject(g_dm.bgBrush);
        if (g_dm.base)  DeleteObject(g_dm.base);
        if (g_dm.small) DeleteObject(g_dm.small);
        g_dm = DlgMetrics();
        return false;
    }
    g_dlg.hwnd = dlg;

    // 标题栏跟随深色（Win11 支持；老系统静默失败，不影响功能）
    typedef HRESULT (WINAPI *DwmSetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        DwmSetAttrFn fn = (DwmSetAttrFn)(void*)GetProcAddress(dwm, "DwmSetWindowAttribute");
        if (fn) {
            BOOL     dark    = TRUE;
            COLORREF capBg   = RGB(38, 40, 45);
            COLORREF capText = RGB(232, 234, 237);
            fn(dlg, 20, &dark,    sizeof(dark));      // DWMWA_USE_IMMERSIVE_DARK_MODE
            fn(dlg, 35, &capBg,   sizeof(capBg));     // DWMWA_CAPTION_COLOR
            fn(dlg, 36, &capText, sizeof(capText));   // DWMWA_TEXT_COLOR
        }
        FreeLibrary(dwm);
    }

    ShowWindow(dlg, SW_SHOW);
    SetFocus(g_dlg.edit);
    EnableWindow(parent, FALSE);
    SetForegroundWindow(dlg);

    // 自检模式：等一拍再拍，免得拍到空白窗口
    if (!g_dlgShotPath.empty()) SetTimer(dlg, ID_TIMER_DLGSHOT, 600, nullptr);

    MSG msg;
    while (!g_dlg.done && GetMessageW(&msg, nullptr, 0, 0)) {
        // 自绘按钮没有 BS_DEFPUSHBUTTON，回车/ESC 自己接
        if (msg.message == WM_KEYDOWN && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            SendMessageW(dlg, WM_COMMAND,
                         MAKEWPARAM(msg.wParam == VK_RETURN ? IDOK : IDCANCEL, BN_CLICKED), 0);
            continue;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    if (g_dm.fieldBrush) DeleteObject(g_dm.fieldBrush);
    if (g_dm.bgBrush)    DeleteObject(g_dm.bgBrush);
    if (g_dm.base)  DeleteObject(g_dm.base);
    if (g_dm.small) DeleteObject(g_dm.small);
    g_dm = DlgMetrics();

    if (g_dlg.ok) {
        g_cfg.intervalMinutes = g_dlg.interval;
        g_cfg.opacityPercent  = g_dlg.opacity;
        g_cfg.topMost = g_dlg.topMost;
        if (g_dlg.autoStart != g_cfg.autoStart) {
            g_cfg.autoStart = g_dlg.autoStart;
            SetAutoStart(g_cfg.autoStart);
        }
        SaveSettings();
        return true;
    }
    return false;
}

// ── 渲染 ─────────────────────────────────────────────────────────────────────
static void RoundedPath(GraphicsPath& p, float x, float y, float w, float h, float r)
{
    float d = r * 2.0f;
    if (w < d * 1.2f || h < d * 1.2f) { p.AddRectangle(RectF(x, y, w, h)); return; }
    p.AddArc(x, y, d, d, 180.0f, 90.0f);
    p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    p.CloseFigure();
}

static void RenderWindow(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;

    // 双缓冲，避免闪烁
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(kBg);

        // 状态点
        Color dot = g_lastError ? kWarn : (g_hasInfo ? kDotOk : kDotIdle);
        SolidBrush dotBrush(dot);
        g.FillEllipse(&dotBrush, (REAL)Scale(11), (REAL)Scale(17), (REAL)Scale(8), (REAL)Scale(8));

        // 两行文字
        Font fontMain(L"Microsoft YaHei UI", (REAL)Scale(15), FontStyleRegular, UnitPixel);
        Font fontSub(L"Microsoft YaHei UI", (REAL)Scale(12), FontStyleRegular, UnitPixel);
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentNear);
        fmt.SetLineAlignment(StringAlignmentCenter);
        fmt.SetTrimming(StringTrimmingEllipsisCharacter);
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);

        std::wstring main, sub;
        Color subColor = kDim;
        if (g_hasInfo) {
            std::wstring place = g_info.region;
            if (!g_info.city.empty()) place += (place.empty() ? L"" : L" ") + g_info.city;
            std::wstring geo = place.empty() ? g_info.country : g_info.country + L" " + place;
            main = g_info.ip + L" · " + geo;
            std::wstring tail = ShortAsn(g_info);
            std::wstring ago = Ago(g_updatedAt);
            if (!tail.empty()) tail += L" · " + ago; else tail = ago;
            if (g_timerWarn) { tail += L" · 定时器异常，已退回 1 分钟"; subColor = kWarn; }
            if (g_lastError) { tail += L"（刷新失败，显示上次结果）"; subColor = kWarn; }
            sub = tail;
        } else if (g_lastError) {
            main = L"出口 IP 获取失败";
            sub = g_lastErrMsg.empty() ? L"稍后自动重试 · 右键可立即刷新"
                                       : g_lastErrMsg + L" · 右键可立即刷新";
            if (sub.size() > 46) sub = sub.substr(0, 46) + L"…";
            subColor = kWarn;
        } else {
            main = L"查询出口 IP 中…";
        }

        SolidBrush textBrush(kText), subBrush(subColor);
        float x = (REAL)Scale(26), cw = (REAL)(w - Scale(36));
        RectF r1(x, (REAL)Scale(8), cw, (REAL)Scale(21));
        RectF r2(x, (REAL)Scale(31), cw, (REAL)Scale(20));
        g.DrawString(main.c_str(), -1, &fontMain, r1, &fmt, &textBrush);
        if (!sub.empty()) g.DrawString(sub.c_str(), -1, &fontSub, r2, &fmt, &subBrush);

        // 圆角描边
        GraphicsPath path;
        RoundedPath(path, 0.5f, 0.5f, (float)w - 1.0f, (float)h - 1.0f, (REAL)Scale(10));
        Pen pen(kBorder, 1.0f);
        g.DrawPath(&pen, &path);
    }

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ── 截图（自检用） ───────────────────────────────────────────────────────────
static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT num = 0, size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;
    std::vector<BYTE> buf(size);
    ImageCodecInfo* info = (ImageCodecInfo*)&buf[0];
    GetImageEncoders(num, size, info);
    for (UINT i = 0; i < num; ++i) {
        if (_wcsicmp(info[i].MimeType, format) == 0) { *pClsid = info[i].Clsid; return (int)i; }
    }
    return -1;
}

static void TakeShot(HWND hwnd, const std::wstring& path)
{
    RECT rc;
    GetWindowRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, rc.left, rc.top, SRCCOPY);
    SelectObject(mem, old);

    std::wstring log = path + L".log";
    {
        Bitmap b(bmp, nullptr);
        CLSID png;
        std::wstring msg;
        bool ok = GetEncoderClsid(L"image/png", &png) >= 0 && b.Save(path.c_str(), &png, nullptr) == Ok;
        // 写 .log 是为了在 -mwindows 下也能看到结果（没有控制台）；只留成功/失败一行
        HANDLE f = CreateFileW(log.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const wchar_t* m = ok ? L"screenshot written" : L"screenshot FAILED";
            DWORD wr = 0;
            WriteFile(f, m, (DWORD)(lstrlenW(m) * sizeof(wchar_t)), &wr, nullptr);
            CloseHandle(f);
        }
    }
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
}

// ── 数据抓取线程 ─────────────────────────────────────────────────────────────
struct FetchResult {
    bool    ok;
    IpInfo  info;
    std::wstring err;
    std::wstring errFull;
};

static unsigned __stdcall FetchThread(void*)
{
    FetchResult* r = new FetchResult();
    r->ok = FetchNow(r->info, r->err, r->errFull);
    if (!PostMessageW(g_hwnd, WM_APP_FETCHED, 0, (LPARAM)r)) delete r;
    return 0;
}

static void StartFetch()
{
    if (g_demo) return;
    // 正在抓取就记账，抓完再补一次；否则“改间隔立即刷新”会被静默吃掉
    if (g_busy) { g_fetchPending = true; return; }
    g_busy = true;
    HANDLE t = (HANDLE)_beginthreadex(nullptr, 0, FetchThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    else g_busy = false;
}

// ── 菜单与操作 ───────────────────────────────────────────────────────────────
static void CopyIp()
{
    if (!g_hasInfo) return;
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t bytes = (g_info.ip.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* p = GlobalLock(mem);
        if (p) { memcpy(p, g_info.ip.c_str(), bytes); GlobalUnlock(mem); SetClipboardData(CF_UNICODETEXT, mem); }
    }
    CloseClipboard();
}

static void ApplyTopMost()
{
    SetWindowPos(g_hwnd, g_cfg.topMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void ResetRefreshTimer()
{
    KillTimer(g_hwnd, ID_TIMER_REFRESH);
    UINT ms = (UINT)g_cfg.intervalMinutes * 60000u;
    if (ms < 1000u) ms = 1000u;
    if (SetTimer(g_hwnd, ID_TIMER_REFRESH, ms, nullptr)) {
        g_timerWarn = false;
        return;
    }
    // 极端情况（句柄耗尽等）：至少每分钟醒一次，并在卡片上说明
    SetTimer(g_hwnd, ID_TIMER_REFRESH, 60000u, nullptr);
    g_timerWarn = true;
}

static void SetInterval(int minutes)
{
    g_cfg.intervalMinutes = minutes;
    SaveSettings();
    ResetRefreshTimer();
    StartFetch();          // 正在抓取时会记账，抓完自动补一次
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

// 从 URL 里取 host 部分，给菜单用（“源 ipinfo.io”）
static std::wstring SourceHost(const std::wstring& url)
{
    size_t p = url.find(L"://");
    if (p == std::wstring::npos) return url;
    p += 3;
    size_t e = url.find_first_of(L"/?#", p);
    std::wstring host = url.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
    if (host.size() > 4 && host.compare(host.size() - 4, 4, L":80") == 0) host.resize(host.size() - 4);
    return host;
}

static void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"立即刷新");
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"设置…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"退出");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

static void ShowContextMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU m = CreatePopupMenu();

    // 第一行：当前状态（不可点）
    std::wstring status;
    if (g_hasInfo) {
        status = g_info.ip;
        if (!g_info.org.empty()) status += L" · " + g_info.org;
        // 数据源也报一下：主源挂了自动切备源时，用户能看出来
        std::wstring host = SourceHost(g_info.source);
        if (!host.empty()) status += L" · 源 " + host;
        status += L" · 更新于 ";
        SYSTEMTIME local;
        SystemTimeToTzSpecificLocalTime(nullptr, &g_info.at, &local);
        wchar_t t[16];
        wsprintfW(t, L"%02d:%02d:%02d", local.wHour, local.wMinute, local.wSecond);
        status += t;
    } else if (g_lastError) {
        status = L"获取失败：" + (g_lastErrFull.empty() ? g_lastErrMsg : g_lastErrFull);
    } else {
        status = L"查询中…";
    }
    if (status.size() > 90) status = status.substr(0, 90) + L"…";
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, status.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"立即刷新");

    HMENU sub = CreatePopupMenu();
    for (int i = 0; i < kIntervalCount; ++i) {
        UINT flags = MF_STRING;
        if (kIntervals[i] == g_cfg.intervalMinutes) flags |= MF_CHECKED;
        wchar_t label[32];
        wsprintfW(label, L"%d 分钟", kIntervals[i]);
        AppendMenuW(sub, flags, IDM_INTERVAL_BASE + i, label);
    }
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"刷新间隔");

    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"设置…");
    AppendMenuW(m, MF_STRING, IDM_COPY, L"复制 IP");
    AppendMenuW(m, MF_STRING | (g_cfg.topMost ? MF_CHECKED : 0), IDM_TOPMOST, L"总在最前");
    AppendMenuW(m, MF_STRING | (g_cfg.autoStart ? MF_CHECKED : 0), IDM_AUTOSTART, L"开机自启");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, IDM_QUIT, L"退出");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    // 把实际生效的配置路径摆出来：降级到便携模式时，不说一声就“丢设置”最让人困惑
    std::wstring cfg = L"配置：" + g_iniPath;
    if (cfg.size() > 72) cfg = cfg.substr(0, 72) + L"…";
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, cfg.c_str());

    SetForegroundWindow(hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(m);
}

static void AddTray(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = kTrayId;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon;
    lstrcpynW(g_nid.szTip, L"ip-float — 出口 IP", 128);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != 0;
}

static void RemoveTray()
{
    if (g_trayAdded) { Shell_NotifyIconW(NIM_DELETE, &g_nid); g_trayAdded = false; }
}

// ── 主窗口过程 ───────────────────────────────────────────────────────────────
static void ApplyAlpha(HWND hwnd)
{
    SetLayeredWindowAttributes(hwnd, 0, (BYTE)(g_cfg.opacityPercent * 255 / 100), LWA_ALPHA);
}

// keepPos != nullptr 时用它当目标位置（DPI 变化时系统建议的新位置），
// 而不是回退到 ini 里可能已经越界的旧坐标。
static void ApplyLayout(HWND hwnd, const RECT* keepPos)
{
    // DPI 变化时窗口还没真的移过去，MonitorFromWindow 会拿到旧显示器，
    // 所以要用系统建议的新位置去反查显示器，否则第一次 WM_DPICHANGED 等于没处理。
    HMONITOR mon = keepPos ? MonitorFromRect(keepPos, MONITOR_DEFAULTTONEAREST)
                           : MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96, dpiY = 96;
    typedef UINT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        GetDpiForMonitorFn fn = (GetDpiForMonitorFn)(void*)GetProcAddress(shcore, "GetDpiForMonitor");
        if (fn) fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dpiX, &dpiY);
        FreeLibrary(shcore);
    }
    g_scale = dpiX / 96.0f;
    if (g_scale < 0.5f || g_scale > 4.0f) g_scale = 1.0f;

    int w = Scale(BASE_W), h = Scale(BASE_H);
    SetWindowPos(hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, Scale(20), Scale(20));
    SetWindowRgn(hwnd, rgn, TRUE);

    ApplyAlpha(hwnd);

    MONITORINFO mi;
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int x, y;
    if (keepPos) {
        x = keepPos->left; y = keepPos->top;
    } else if (g_cfg.x != -1 && g_cfg.y != -1) {
        x = g_cfg.x; y = g_cfg.y;
    } else {
        x = mi.rcWork.right - w - Scale(24);
        y = mi.rcWork.top + Scale(24);
    }
    // DPI 变大 / 屏幕变小时可能超界，拉回来而不是让它跑出可视区
    if (x + w > mi.rcWork.right)  x = mi.rcWork.right - w;
    if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
    if (x < mi.rcWork.left)       x = mi.rcWork.left;
    if (y < mi.rcWork.top)        y = mi.rcWork.top;
    SetWindowPos(hwnd, g_cfg.topMost ? HWND_TOPMOST : HWND_NOTOPMOST, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
    // 注意：这里不能带 SWP_SHOWWINDOW —— ApplyLayout 现在也会被 WM_DPICHANGED 调用，
    // 带上它会把用户在托盘里隐藏的卡片又给显示出来。
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        return 0;

    case WM_PAINT:
        RenderWindow(hwnd);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DPICHANGED: {
        // 换屏 / 改缩放比：按新 DPI 重算尺寸、圆角、透明度，位置用系统建议值
        ApplyLayout(hwnd, (const RECT*)lp);
        return 0;
    }

    case WM_APP_ACTIVATE:
        // 第二个实例启动了：把已有卡片亮一下再退出。
        // 用定长计时器驱动闪烁 —— 卡片是 NOACTIVATE 窗口，FlashWindow(TRUE) 自己不会停。
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        g_flashTicks = 8;
        SetTimer(hwnd, ID_TIMER_FLASH, 250, nullptr);
        return 0;

    case WM_TIMER:
        if (wp == ID_TIMER_REFRESH) {
            StartFetch();
        } else if (wp == ID_TIMER_TICK) {
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (wp == ID_TIMER_SHOT) {
            KillTimer(hwnd, ID_TIMER_SHOT);
            TakeShot(hwnd, g_shotPath);
            DestroyWindow(hwnd);
        } else if (wp == ID_TIMER_FLASH) {
            FlashWindow(hwnd, TRUE);
            if (--g_flashTicks <= 0 || GetForegroundWindow() == hwnd)
                KillTimer(hwnd, ID_TIMER_FLASH);
        }
        return 0;

    case WM_APP_FETCHED: {
        FetchResult* r = (FetchResult*)lp;
        g_busy = false;
        if (r) {
            if (r->ok) {
                g_info = r->info;
                g_updatedAt = r->info.at;
                g_hasInfo = true;
                g_lastError = false;
                g_lastErrMsg.clear();
                g_lastErrFull.clear();
            } else {
                g_lastError = true;
                g_lastErrMsg = r->err;
                g_lastErrFull = r->errFull;
            }
            delete r;
        }
        // 抓取途中又收到过刷新请求（改了间隔 / 点了立即刷新）：补一次
        if (g_fetchPending && !g_demo) {
            g_fetchPending = false;
            StartFetch();
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        if (!g_shotPath.empty() && !g_shotDone) {
            g_shotDone = true;
            SetTimer(hwnd, ID_TIMER_SHOT, 700, nullptr);
        }
        return 0;
    }

    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        } else if (LOWORD(lp) == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, IsWindowVisible(hwnd) ? SW_HIDE : SW_SHOW);
        }
        return 0;

    case WM_RBUTTONUP:
        ShowContextMenu(hwnd);
        return 0;

    case WM_LBUTTONDBLCLK:
        StartFetch();
        return 0;

    case WM_LBUTTONDOWN:
        ReleaseCapture();
        SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;

    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDM_REFRESH) {
            StartFetch();
        } else if (id >= IDM_INTERVAL_BASE && id < IDM_INTERVAL_BASE + kIntervalCount) {
            SetInterval(kIntervals[id - IDM_INTERVAL_BASE]);
        } else if (id == IDM_SETTINGS) {
            if (ShowSettings(hwnd)) {
                ApplyTopMost();
                ApplyAlpha(hwnd);      // 不透明度可能刚被改过
                ResetRefreshTimer();
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (id == IDM_COPY) {
            CopyIp();
        } else if (id == IDM_TOPMOST) {
            g_cfg.topMost = !g_cfg.topMost;
            SaveSettings();
            ApplyTopMost();
        } else if (id == IDM_AUTOSTART) {
            g_cfg.autoStart = !g_cfg.autoStart;
            SetAutoStart(g_cfg.autoStart);
            SaveSettings();
        } else if (id == IDM_QUIT) {
            DestroyWindow(hwnd);
        }
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY: {
        RECT rc;
        GetWindowRect(hwnd, &rc);
        g_cfg.x = rc.left;
        g_cfg.y = rc.top;
        SaveSettings();
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── 入口 ─────────────────────────────────────────────────────────────────────
int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int)
{
    g_inst = inst;

    // DPI：优先 Per-Monitor V2
    typedef BOOL (WINAPI *SetDpiCtxFn)(void*);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        SetDpiCtxFn fn = (SetDpiCtxFn)(void*)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn) fn((void*)-4);            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        else SetProcessDPIAware();
    }

    // 命令行
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int intervalOverride = 0;
    std::wstring sourceOverride;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--demo") {
            g_demo = true;
        } else if (a == L"--shot" && i + 1 < argc) {
            g_shotPath = argv[++i];
        } else if (a == L"--shot-dlg" && i + 1 < argc) {
            g_dlgShotPath = argv[++i];
            g_demo = true;                 // 自检模式不联网
        } else if (a == L"--source" && i + 1 < argc) {
            sourceOverride = argv[++i];
        } else if (a == L"--interval" && i + 1 < argc) {
            intervalOverride = _wtoi(argv[++i]);
        }
    }
    if (argv) LocalFree(argv);

    LoadSettings(g_cfg);
    // 开机自启以注册表为准：手工删过 Run 项、或 ini 写失败时，状态才不会骗人
    g_cfg.autoStart = AutoStartEnabled();
    if (intervalOverride >= 1 && intervalOverride <= 1440) g_cfg.intervalMinutes = intervalOverride;

    if (!sourceOverride.empty()) {
        g_sources.push_back(sourceOverride);
    } else {
        g_sources.push_back(L"https://ipinfo.io/json");
        g_sources.push_back(L"https://ipapi.co/json/");
    }

    g_icon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                               GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!g_icon) g_icon = LoadIconW(nullptr, IDI_APPLICATION);

    GdiplusStartupInput gdiIn;
    GdiplusStartup(&g_gdiToken, &gdiIn, nullptr);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    wc.hIcon = g_icon;
    wc.lpszClassName = kWndClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW dc;
    ZeroMemory(&dc, sizeof(dc));
    dc.cbSize = sizeof(dc);
    dc.lpfnWndProc = DlgProc;
    dc.hInstance = inst;
    dc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    dc.lpszClassName = kDlgClass;
    RegisterClassExW(&dc);

    // 单实例：已有实例在跑就把它的卡片闪一下然后退出。
    // 自检模式（--demo / --shot / --shot-dlg）跳过，否则没法在运行时验证渲染。
    bool selfTest = g_demo || !g_shotPath.empty() || !g_dlgShotPath.empty();
    if (!selfTest) {
        g_mutex = CreateMutexW(nullptr, TRUE, kMutexName);
        if (g_mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND other = FindWindowW(kWndClass, nullptr);
            if (other) PostMessageW(other, WM_APP_ACTIVATE, 0, 0);
            CloseHandle(g_mutex);
            g_mutex = nullptr;
            return 0;
        }
        // g_mutex 从此不再关闭：进程退出时由系统释放，别人也就拿到了“已退出”信号
    }

    HWND hwnd = CreateWindowExW(
        // NOACTIVATE：点卡片不再抢走你正在打的字；但拖动、右键菜单、双击刷新都不受影响
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        kWndClass, L"ip-float",
        WS_POPUP,
        0, 0, Scale(BASE_W), Scale(BASE_H),
        nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    ApplyLayout(hwnd, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    AddTray(hwnd);

    if (g_demo) {
        g_info = IpInfo();
        g_info.ip = L"203.0.113.42";
        g_info.country = L"JP";
        g_info.region = L"Tokyo";
        g_info.city = L"Chiyoda";
        g_info.asn = L"AS20473";
        g_info.org = L"The Constant Company, LLC";
        g_info.source = L"https://ipinfo.io/json";
        GetSystemTime(&g_info.at);
        {   // 让自检图看起来像「2 分钟前」，而不是刚刷新过
            FILETIME ft;
            SystemTimeToFileTime(&g_info.at, &ft);
            ULARGE_INTEGER u;
            u.LowPart = ft.dwLowDateTime;
            u.HighPart = ft.dwHighDateTime;
            u.QuadPart -= 130ULL * 10000000ULL;
            ft.dwLowDateTime = u.LowPart;
            ft.dwHighDateTime = u.HighPart;
            FileTimeToSystemTime(&ft, &g_info.at);
        }
        g_updatedAt = g_info.at;
        g_hasInfo = true;
        InvalidateRect(hwnd, nullptr, FALSE);
        if (!g_shotPath.empty()) {
            g_shotDone = true;
            SetTimer(hwnd, ID_TIMER_SHOT, 700, nullptr);
        }
        if (!g_dlgShotPath.empty()) {
            // 设置窗口自检：ShowSettings 内部会定时拍图并自己关闭
            ShowSettings(hwnd);
            PostQuitMessage(0);
        }
    } else {
        StartFetch();
        ResetRefreshTimer();
        SetTimer(hwnd, ID_TIMER_TICK, 30000, nullptr);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    RemoveTray();
    if (g_icon) DestroyIcon(g_icon);
    GdiplusShutdown(g_gdiToken);
    if (g_mutex) CloseHandle(g_mutex);
    return 0;
}
