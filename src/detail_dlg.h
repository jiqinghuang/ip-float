#pragma once

#include <windows.h>

#include <cstdint>

#include "probe.h"

// 诊断窗口 → 主窗口的通知。窗口自己不抓数据，只负责显示和转发。
const UINT WM_DIAG_REFRESH = WM_APP + 10;   // 用户点了「刷新」
const UINT WM_DIAG_CLOSED  = WM_APP + 11;   // 用户把窗口关掉了（主窗口据此写 panelOpen=0）

// 打开或复用窗口。第一次打开时立刻显示传入的结果（可能是上一次的），不等网络。
void ShowDiagWindow(HWND appHwnd, const Diagnosis& d, uint64_t at);

// 结果回来了。窗口没开就什么都不做。
void UpdateDiagWindow(const Diagnosis& d, uint64_t at);

// 探测进行中：按钮变灰、底部显示「检测中…」
void SetDiagBusy(bool busy);

bool DiagWindowOpen();
void CloseDiagWindow();

// 离屏把诊断面板画成 PNG（不建窗口、不读系统、不碰配置）。
// 网页素材靠它：和窗口用同一段排版与绘制代码，不会两边长得不一样。
bool RenderDiagToPng(const std::wstring& path, float scale, const Diagnosis& d, uint64_t at,
                     std::wstring& err);
