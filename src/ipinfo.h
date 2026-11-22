#pragma once

#include <cstdint>
#include <string>

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
};

// 从一段 JSON 里取字符串字段；找不到返回空串。够 ipinfo/ipapi 两个固定源用。
std::string JsonStr(const std::string& json, const std::string& key);

bool ParseIpInfo(const std::string& json, const std::wstring& source, IpInfo& out,
                 std::wstring& err);

// 把 "AS20473 The Constant Company, LLC" 压成 "AS20473 Vultr"
std::wstring ShortAsn(const IpInfo& info);
