#include "config.h"

#include "util.h"

#include <windows.h>

#include <cwctype>
#include <string>

namespace {

const wchar_t* kSection = L"ip-float";

std::wstring TrimText(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) ++a;
    while (b > a && iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring GetEnv(const wchar_t* name)
{
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return std::wstring();
    std::wstring out(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, out.data(), n);
    if (got == 0 || got >= n) return std::wstring();
    out.resize(got);
    return out;
}

bool FileExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool DirWritable(const std::wstring& dir)
{
    std::wstring probe = dir + L"\\.ip-float-write-test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// 只在降级到便携模式时用：把 APPDATA 里的老配置带过来，避免用户“丢设置”
bool CopyIni(const std::wstring& from, const std::wstring& to)
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

// --ini 指向的目录可能还不存在；逐级创建，避免 SaveConfig 静默失败
void EnsureParentDir(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos || p == 0) return;
    std::wstring dir = path.substr(0, p);
    if (dir.empty()) return;

    std::wstring cur;
    for (size_t i = 0; i < dir.size(); ++i) {
        cur += dir[i];
        if (dir[i] == L'\\' || dir[i] == L'/') {
            if (cur.size() > 3) CreateDirectoryW(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryW(dir.c_str(), nullptr);
}

} // namespace

IniLocation ResolveIniPath(const std::wstring& overridePath)
{
    if (!overridePath.empty()) {
        EnsureParentDir(overridePath);
        return IniLocation{ overridePath, false };
    }

    std::wstring appdata = GetEnv(L"APPDATA");
    std::wstring portable = ExeDir() + L"\\ip-float.ini";

    if (!appdata.empty()) {
        std::wstring dir = appdata + L"\\ip-float";
        CreateDirectoryW(dir.c_str(), nullptr);
        if (DirWritable(dir)) {
            return IniLocation{ dir + L"\\settings.ini", false };
        }

        // 降级了：APPDATA 里已经有配置就搬过来，否则用户会“丢设置”而不自知。
        // 已经有便携配置则不动（不覆盖）。
        std::wstring old = dir + L"\\settings.ini";
        if (FileExists(old) && !FileExists(portable)) CopyIni(old, portable);
    }
    return IniLocation{ portable, true };
}

bool LoadConfig(const std::wstring& path, Config& cfg)
{
    Config s;
    const wchar_t* p = path.c_str();

    s.intervalMinutes = (int)GetPrivateProfileIntW(kSection, L"intervalMinutes", 5, p);
    s.opacityPercent  = (int)GetPrivateProfileIntW(kSection, L"opacityPercent", 94, p);
    s.topMost         = GetPrivateProfileIntW(kSection, L"topMost", 1, p) != 0;
    s.autoStart       = GetPrivateProfileIntW(kSection, L"autoStart", 0, p) != 0;
    s.x               = (int)GetPrivateProfileIntW(kSection, L"x", -1, p);
    s.y               = (int)GetPrivateProfileIntW(kSection, L"y", -1, p);
    s.panelOpen       = GetPrivateProfileIntW(kSection, L"panelOpen", 0, p) != 0;
    s.ignoreTimezone  = GetPrivateProfileIntW(kSection, L"ignoreTimezone", 0, p) != 0;

    wchar_t src[2048] = {};
    GetPrivateProfileStringW(kSection, L"source", L"", src, 2048, p);
    s.source = src;

    wchar_t expectCountry[64] = {};
    GetPrivateProfileStringW(kSection, L"expectedCountry", L"", expectCountry, 64, p);
    s.expectedCountry = TrimText(expectCountry);

    wchar_t expectAsn[64] = {};
    GetPrivateProfileStringW(kSection, L"expectedAsn", L"", expectAsn, 64, p);
    s.expectedAsn = TrimText(expectAsn);

    if (s.intervalMinutes < 1) s.intervalMinutes = 1;
    if (s.intervalMinutes > 1440) s.intervalMinutes = 1440;
    if (s.opacityPercent < 0) s.opacityPercent = 0;
    if (s.opacityPercent > 100) s.opacityPercent = 100;

    cfg = s;
    return true;
}

bool SaveConfig(const std::wstring& path, const Config& cfg)
{
    if (path.empty()) return false;
    EnsureParentDir(path);

    bool ok = true;
    auto put = [&](const wchar_t* key, const std::wstring& value) {
        if (!WritePrivateProfileStringW(kSection, key, value.c_str(), path.c_str()))
            ok = false;
    };

    put(L"intervalMinutes", std::to_wstring(cfg.intervalMinutes));
    put(L"opacityPercent",  std::to_wstring(cfg.opacityPercent));
    put(L"topMost",         cfg.topMost ? L"1" : L"0");
    put(L"autoStart",       cfg.autoStart ? L"1" : L"0");
    put(L"x",               std::to_wstring(cfg.x));
    put(L"y",               std::to_wstring(cfg.y));
    put(L"panelOpen",       cfg.panelOpen ? L"1" : L"0");
    put(L"source",          cfg.source);   // 空值 = 删除该键，恢复内置源
    put(L"expectedCountry", cfg.expectedCountry);
    put(L"expectedAsn",     cfg.expectedAsn);
    put(L"ignoreTimezone", cfg.ignoreTimezone ? L"1" : L"0");

    return ok;
}

bool AutoStartEnabled()
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;

    wchar_t val[MAX_PATH * 2] = {};
    DWORD cb = sizeof(val);
    bool on = RegQueryValueExW(k, L"ip-float", nullptr, nullptr, (LPBYTE)val, &cb) == ERROR_SUCCESS
           && val[0] != 0;
    RegCloseKey(k);
    return on;
}

bool SetAutoStart(bool on)
{
    HKEY k = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return false;

    LSTATUS st = ERROR_SUCCESS;
    if (on) {
        const std::wstring exe = ExePath();
        if (exe.empty()) {
            RegCloseKey(k);
            return false;
        }
        std::wstring quoted = std::wstring(L"\"") + exe + L"\"";
        st = RegSetValueExW(k, L"ip-float", 0, REG_SZ,
                            (const BYTE*)quoted.c_str(),
                            (DWORD)((quoted.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(k, L"ip-float");
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(k);
    return st == ERROR_SUCCESS;
}
