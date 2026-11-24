#pragma once

#include <string>

struct Config {
    int          intervalMinutes = 5;
    int          opacityPercent  = 94;
    bool         topMost         = true;
    bool         autoStart       = false;
    int          x               = -1;
    int          y               = -1;
    bool         panelOpen       = false;   // 上次退出时诊断窗口是开着还是关着
    std::wstring source;          // 空 = 用内置的 ipinfo/ipapi 双源

    // 可选期望出口：空 = 不检查。国家码如 JP；ASN 可写 20473 或 AS20473。
    std::wstring expectedCountry;
    std::wstring expectedAsn;

    // x / y == -1 表示还没摆过，由 ApplyLayout 放到右上角。
    // 用 -1 而不是 >= 0 判断，是为了允许负坐标（多屏布局里卡片在左侧屏上）。
};

struct IniLocation {
    std::wstring path;
    bool         portable = false;   // true = 退到了 exe 同目录
};

// 显式覆盖（--ini）优先；否则 %APPDATA%\ip-float\settings.ini；
// 写不进去才退到 exe 同目录的 ip-float.ini（便携用法）。
IniLocation ResolveIniPath(const std::wstring& overridePath);

bool LoadConfig(const std::wstring& path, Config& cfg);
bool SaveConfig(const std::wstring& path, const Config& cfg);

// 注册表是「开机自启」的唯一真相：ini 只是缓存。
bool AutoStartEnabled();
bool SetAutoStart(bool on);
