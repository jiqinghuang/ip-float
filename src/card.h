#pragma once

#include <windows.h>
#include <gdiplus.h>

#include <cstdint>
#include <string>
#include <vector>

#include "ipinfo.h"
#include "probe.h"

enum class DotState { Idle, Ok, Warn, Bad };

struct CardRect {
    int x = 0, y = 0, w = 0, h = 0;
};

// 所有卡片几何都在这里算一次；渲染、窗口 region、--render sidecar 共用。
struct CardMetrics {
    float    scale = 1.0f;
    int      width = 0, height = 0;
    int      dotX = 0, dotY = 0, dotSize = 0;
    int      mainFontPx = 0, subFontPx = 0, badgeFontPx = 0;
    CardRect line1, line2, line3;
    int      radius = 0;      // 圆角半径（不是直径）
};

struct CardText {
    std::wstring main;
    std::wstring sub;
    bool         subWarn = false;
    DotState     dot = DotState::Idle;
    std::vector<Badge> badges;   // 第三行；空 = 不画这一行
};

CardMetrics ComputeCardMetrics(float scale);
CardText    BuildCardText(const IpInfo& info, bool hasInfo, bool lastError, bool timerWarn,
                          const std::wstring& errMsg, uint64_t updatedAt, uint64_t now);

// 画卡片；transparentCorners = true 时用透明底 + 抗锯齿圆角（--render 用）
void DrawCard(Gdiplus::Graphics& g, const CardMetrics& m, const CardText& t,
              bool transparentCorners);

// 离屏渲染成 PNG（不受桌面、壁纸、窗口不透明度影响），并写 sidecar .json + .log
bool RenderCardToPng(const std::wstring& path, const CardMetrics& m, const CardText& t,
                     std::wstring& err);

// 屏幕抓窗口（--shot / --shot-dlg 自检用），并写 .log
bool CaptureWindowToPng(HWND hwnd, const std::wstring& path, std::wstring& err);

// 把一张 GDI+ 位图存成 PNG（诊断窗口离屏渲染也用这个）
bool SavePngToFile(Gdiplus::Bitmap& bmp, const std::wstring& path, std::wstring& err);

void RoundedPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r);
