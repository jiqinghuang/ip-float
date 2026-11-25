#pragma once

#include <cstdint>
#include <string>

#include "json.h"

// 数据源没给 UTC 偏移时的哨兵值（不能和任何真实偏移撞车）
const int kNoOffset = 0x7fffffff;

// 纯数据 + 纯解析：不依赖窗口、GDI、WinHTTP，方便单测。
struct IpInfo {
    std::wstring ip;
    std::wstring country;
    std::wstring region;
    std::wstring city;
    std::wstring asn;
    std::wstring org;
    std::wstring source;
    uint64_t     at = 0;   // 抓取时间（FILETIME, UTC）
    int          utcOffsetMinutes = kNoOffset;   // 该 IP 所在时区相对 UTC（分钟）
};

bool ParseIpInfo(const std::string& json, const std::wstring& source, IpInfo& out,
                 std::wstring& err);

// 把 "AS20473 The Constant Company, LLC" 压成 "AS20473 Vultr"
std::wstring ShortAsn(const IpInfo& info);

// "+0800" / "-0500" → 分钟；不合法返回 kNoOffset
int ParseUtcOffset(const std::string& s);
