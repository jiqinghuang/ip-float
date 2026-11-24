#pragma once

#include <string>
#include <vector>

#include "ipinfo.h"
#include "netinfo.h"

// 严重度。Skip 表示「没测/读不到」，它**不是**通过。
enum class Sev { Ok, Warn, Bad, Skip };

enum class LineId { Tunnel, V6Route, Proxy, ExitV4, ExitV6, Asn, Tz, Dns };

struct ProbeLine {
    LineId       id    = LineId::Tunnel;
    std::wstring label;
    std::wstring value;
    Sev          sev   = Sev::Skip;
    std::wstring note;          // 依据 / 补充说明，可为空
};

// 一次出口探测的结果（v4 或 v6 各一份）
const int kUnknownOffset = kNoOffset;

struct ExitProbe {
    bool         ok = false;
    std::wstring ip;
    std::wstring country, region, city;
    std::wstring asn, org;
    bool         isHosting = false;
    bool         isProxy   = false;
    bool         isVpn     = false;
    int          utcOffsetMinutes = kUnknownOffset;
    std::wstring err;
};

struct ProbeInput {
    NetFacts     facts;
    ExitProbe    v4;
    ExitProbe    v6;
    bool         dnsChecked = false;
    std::wstring dnsResolverIp;
};

struct Diagnosis {
    Sev                    verdict = Sev::Skip;
    std::wstring           summary;
    std::wstring           evidence;
    std::vector<ProbeLine> lines;

    const ProbeLine* Find(LineId id) const
    {
        for (const auto& l : lines) if (l.id == id) return &l;
        return nullptr;
    }
};

// 纯函数：不联网、不读系统。全部输入来自 ProbeInput，方便单测。
Diagnosis Diagnose(const ProbeInput& in);

// 卡片第三行的 5 个徽章，顺序固定：TUN / V6 / 代理 / 时区 / DNS
struct Badge {
    std::wstring label;
    Sev          sev = Sev::Skip;
};
std::vector<Badge> BuildBadges(const Diagnosis& d);

// 文本输出用（ASCII，避免任何字体问题）
const char* SevTag(Sev s);
