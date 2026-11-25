#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>

struct Tray {
    NOTIFYICONDATAW nid = {};
    bool added = false;
};

void TrayInit(Tray& tray, HWND hwnd, HICON icon, UINT callbackMsg, const wchar_t* tip);
void TraySetTip(Tray& tray, const std::wstring& tip);
bool TrayAdd(Tray& tray);
void TrayRemove(Tray& tray);

// Explorer 重启后会广播 TaskbarCreated；收到后必须重新 NIM_ADD，图标才会回来。
UINT TrayTaskbarCreatedMessage();
