#pragma once

#include <string>
#include <vector>

#include "ipinfo.h"
#include "netinfo.h"

// 严重度。Skip 表示「没测/读不到」，它**不是**通过。
enum class Sev { Ok, Warn, Bad, Skip };

enum class LineId { Tunnel, V6Route, Proxy, ExitV4, ExitV6, V6Local, Asn, Expect, Tz, Dns };

struct ProbeLine {
    LineId       id    = LineId::Tunnel;
    std::wstring label;
    std::wstring value;
    Sev          sev   = Sev::Skip;
    bool         na    = false; // true = 这个检查在当前配置下不适用（≠ 没测到）
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
    bool         tlsLenient = false;   // 严格校验失败后降级才拿到结果
    std::wstring err;
};

struct ProbeInput {
    NetFacts     facts;
    ExitProbe    v4;
    ExitProbe    v6;
    bool         dnsChecked = false;
    std::wstring dnsResolverIp;
    std::wstring dnsResolverCountry;   // 解析器出口 IP 的归属国（可能为空）
    std::wstring dnsResolverOrg;       // 解析器出口 IP 的运营商，用来识别公共 DNS
    std::wstring dnsError;             // 查询失败时的原因（错误码），空 = 没失败

    // 可选期望出口：空 = 不检查。配置后诊断里多一行「期望」，
    // 不匹配只影响总判定和诊断窗口，不在卡片上新增第六个徽章。
    std::wstring expectedCountry;      // 国家码，例如 JP
    std::wstring expectedAsn;          // 规范化为 AS20473

    // true = 不把「IP 时区 ≠ 本机时区」当成可疑项（远程办公/跨时区常驻）
    bool         ignoreTimezone = false;
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

// 卡片第三行的 5 个徽章，顺序固定：TUN / V6 / 路径 / 时区 / DNS
struct Badge {
    std::wstring label;
    Sev          sev = Sev::Skip;
    bool         na  = false;   // 不适用：卡片画空心点
};
std::vector<Badge> BuildBadges(const Diagnosis& d);

// 文本输出用（ASCII，避免任何字体问题）
const char* SevTag(Sev s);
