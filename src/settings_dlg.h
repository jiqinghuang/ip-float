#pragma once

#include <windows.h>
#include <string>

#include "config.h"

struct SettingsDialogInput {
    Config       cfg;
    float        scale = 1.0f;
    std::wstring shotPath;   // 非空：--shot-dlg 自检，拍完自动关闭
};

struct SettingsDialogResult {
    bool         ok = false;
    int          interval = 5;
    int          opacity = 94;
    bool         topMost = true;
    bool         autoStart = false;
    std::wstring expectedCountry;
    std::wstring expectedAsn;
    bool         ignoreTimezone = false;
    bool         shotFailed = false;   // --shot-dlg 抓图失败
};

bool ShowSettingsDialog(HWND parent, const SettingsDialogInput& in,
                        SettingsDialogResult& out);
