#pragma once

#include <string>

#include "ipinfo.h"

namespace selftest {

struct DemoCard {
    IpInfo   info;
    uint64_t updatedAt = 0;
};

DemoCard MakeDemoCard(uint64_t now);

// 离屏渲染一张演示卡片；不联网、不建窗口、不读写用户配置
bool RenderDemoCard(const std::wstring& path, float scale, std::wstring& err);

} // namespace selftest
