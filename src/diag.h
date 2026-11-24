#pragma once

#include <string>

#include "ipinfo.h"
#include "probe.h"

// --diag <path>：采集本机事实 → 探测出口 → 判定 → 写 .txt（+ .json sidecar）。
// 不建窗口、不读写配置，供自检和 CI 用。
bool RunDiag(const std::wstring& outPath, std::wstring& err);

// 卡片自动轮询用：拿已有的 IpInfo 当 v4 基准，补上本机事实、时区偏移和 v6 出口。
// 只读系统 + 短超时请求，失败只影响对应徽章，不影响主流程。
// expectedCountry / expectedAsn 为空表示不检查期望出口。
bool ProbeExits(const IpInfo& base, ProbeInput& out,
                const std::wstring& expectedCountry = std::wstring(),
                const std::wstring& expectedAsn = std::wstring());
