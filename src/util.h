#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

// 纯工具：字符串、时间、DPI、URL。这里不放任何 UI 或网络逻辑。

// 地址族判定。用 InetPtonW 而不是看冒号，避免把 "2001:db8::zz" 当 v6。
enum class IpKind { None, V4, V6 };
IpKind ClassifyIp(const std::wstring& text);

// "JP / AS20473"、"jp,20473"、"20473" → 国家码 / ASN。纯字符串，不校验数据源。
void ParseExpectedText(const std::wstring& text, std::wstring& country, std::wstring& asn);

std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& s);

std::wstring ExePath();
std::wstring ExeDir();
uint64_t     NowFileTime();
std::wstring Ago(uint64_t then, uint64_t now);
std::wstring LocalTimeString(uint64_t fileTimeUtc);
bool         FileTimeMinusSeconds(uint64_t base, uint64_t seconds, uint64_t& out);

int   ScalePx(int v, float scale);
float DpiScaleForMonitor(HMONITOR monitor);

// "https://ipinfo.io/json" -> "ipinfo.io"（去掉默认端口），菜单展示用
std::wstring SourceHost(const std::wstring& url);
