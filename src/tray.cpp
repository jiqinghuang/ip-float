#include "tray.h"

namespace {
const UINT kTrayId = 1;
}

void TrayInit(Tray& tray, HWND hwnd, HICON icon, UINT callbackMsg, const wchar_t* tip)
{
    ZeroMemory(&tray.nid, sizeof(tray.nid));
    tray.nid.cbSize = sizeof(tray.nid);
    tray.nid.hWnd = hwnd;
    tray.nid.uID = kTrayId;
    tray.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray.nid.uCallbackMessage = callbackMsg;
    tray.nid.hIcon = icon;
    lstrcpynW(tray.nid.szTip, tip, 128);
    tray.added = false;
}

void TraySetTip(Tray& tray, const std::wstring& tip)
{
    lstrcpynW(tray.nid.szTip, tip.c_str(), 128);
    if (tray.added) {
        // 只改 tip；图标/消息回调沿用 TrayInit 的设置
        tray.nid.uFlags = NIF_TIP;
        Shell_NotifyIconW(NIM_MODIFY, &tray.nid);
        tray.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    }
}

bool TrayAdd(Tray& tray)
{
    tray.added = Shell_NotifyIconW(NIM_ADD, &tray.nid) != 0;
    return tray.added;
}

void TrayRemove(Tray& tray)
{
    if (tray.added) {
        Shell_NotifyIconW(NIM_DELETE, &tray.nid);
        tray.added = false;
    }
}

UINT TrayTaskbarCreatedMessage()
{
    static const UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
    return msg;
}
