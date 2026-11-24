#pragma once

#include <string>

#include "ipinfo.h"
#include "probe.h"

namespace selftest {

struct DemoCard {
    IpInfo   info;
    uint64_t updatedAt = 0;
};

DemoCard MakeDemoCard(uint64_t now);

// 离屏渲染一张演示卡片；不联网、不建窗口、不读写用户配置
bool RenderDemoCard(const std::wstring& path, float scale, std::wstring& err);

// 演示用的诊断结论（全部正常），给网页素材和 --render-dlg 用
Diagnosis MakeDemoDiagnosis(uint64_t now);

} // namespace selftest
