#pragma once

#include <string>

// GET 一个 URL，成功时 body 为响应体（上限 1 MiB）。
// 失败时 err 是给用户看的中文原因（含 WinHTTP 错误码 / HTTP 状态码）。
// timeoutMs <= 0 时用默认超时（8s）。探测类请求会传更短的值，避免拖慢刷新。
bool HttpGet(const std::wstring& url, std::string& body, std::wstring& err, int timeoutMs = 0);
