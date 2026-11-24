#pragma once

#include <string>

// GET 一个 URL，成功时 body 为响应体（上限 1 MiB）。
// 失败时 err 是给用户看的中文原因（含 WinHTTP 错误码 / HTTP 状态码）。
// timeoutMs <= 0 时用默认超时（8s）。探测类请求会传更短的值，避免拖慢刷新。
// direct = true 时**绕过系统代理**，只走路由表（测 TUN 隧道必须这样，
// 否则 IPv6 字面量会被 HTTP 代理的 CONNECT 卡住，误报「不可达」）。
bool HttpGet(const std::wstring& url, std::string& body, std::wstring& err,
             int timeoutMs = 0, bool direct = false);
