#pragma once

#include <string>

// 只「读取系统事实」，不做任何判定。
// 故意不 include windows.h —— 让 probe.cpp / 单测能轻量引用这份结构。
struct NetFacts {
    // ── 系统代理（HKCU\...\Internet Settings）─────────────────────────
    bool         proxyEnabled    = false;   // ProxyEnable == 1
    bool         proxyConfigured = false;   // 有 ProxyServer 或 AutoConfigURL
    std::wstring proxyServer;               // 例如 "127.0.0.1:10808"
    bool         pacConfigured   = false;   // AutoConfigURL 非空（PAC 优先级高于 ProxyServer）

    // ── 隧道 ──────────────────────────────────────────────────────────
    // 「是否接管」= 到目标的默认路由是否落在**物理网卡之外**的接口上。
    // 用路由判定而不是网卡名匹配：名字可以随便改，路由表不会说谎。
    bool         tunDefaultV4 = false;
    bool         tunDefaultV6 = false;
    std::wstring tunAdapter;                // 接管 v4 的接口描述（空 = 没有）
    std::wstring tunIfName;

    // ── 本机地址 ──────────────────────────────────────────────────────
    std::wstring lanIp;                     // 主物理网卡的 IPv4
    std::wstring lanAdapter;
    bool         hasPublicV6 = false;       // 有全局（非 link-local / 非 ULA）IPv6
    std::wstring publicV6;

    // ── 时区 ──────────────────────────────────────────────────────────
    int          localOffsetMinutes = 0;    // 相对 UTC，东八区 = 480
    std::wstring localTzName;

    // ── 定位服务 ──────────────────────────────────────────────────────
    int          locationConsent = -1;      // -1 未知 / 0 关 / 1 开

    std::wstring err;                       // 采集失败原因（空 = 正常）
};

// 全量采集；个别字段读不到只留空，不整体失败。
bool CollectNetFacts(NetFacts& out);
