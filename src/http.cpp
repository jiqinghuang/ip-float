#include "http.h"

#include "version.h"

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <vector>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif

namespace {

const DWORD kTimeoutMs = 8000;
const size_t kMaxBody = 1u << 20;   // 1 MiB 上限，防跑飞

// 只负责关句柄，避免早期 return 漏 CloseHandle
struct WinHttpHandle {
    HINTERNET h = nullptr;

    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET v) : h(v) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& o) noexcept : h(o.h) { o.h = nullptr; }
    WinHttpHandle& operator=(WinHttpHandle&& o) noexcept
    {
        if (this != &o) {
            if (h) WinHttpCloseHandle(h);
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }

    explicit operator bool() const { return h != nullptr; }
    HINTERNET get() const { return h; }
};

std::wstring WinHttpErrorText(DWORD code)
{
    const wchar_t* hint = nullptr;
    switch (code) {
    case 12002: hint = L"超时"; break;
    case 12007: hint = L"域名解析失败（DNS 或网络不通）"; break;
    case 12029: hint = L"无法连接（端口不通、被墙，或该走代理）"; break;
    case 12030: hint = L"连接被中断"; break;
    case 12031: hint = L"连接被重置"; break;
    case 12175: hint = L"TLS 握手失败"; break;
    case 12182: hint = L"证书颁发机构不受信任"; break;
    case 12183: hint = L"证书域名不匹配"; break;
    case 12184: hint = L"证书已过期"; break;
    case 12185: hint = L"证书用途不匹配（常见于代理/中间人拦截）"; break;
    case 12186: hint = L"证书无效"; break;
    case 12187: hint = L"证书吊销状态无法确认"; break;
    default: break;
    }
    wchar_t buf[160] = {};
    if (hint) swprintf(buf, 160, L"%u：%s", (unsigned)code, hint);
    else      swprintf(buf, 160, L"%u", (unsigned)code);
    return buf;
}

// ipinfo.io 未注册 token 时限流很凶，429 必须说清楚，否则用户只会看到干巴巴一个数字
std::wstring HttpStatusText(DWORD status)
{
    switch (status) {
    case 401: return L"401：数据源要求认证（ipinfo 需要带 token，换 --source 或用 ipapi.co）";
    case 403: return L"403：被拒绝（数据源要 token，或你的出口 IP 被限流封禁）";
    case 404: return L"404：地址不存在（--source 写错了？）";
    case 429: return L"429：请求太频繁，被数据源限流（调大间隔，或用 --source 换源）";
    case 500: return L"500：数据源自己出错了，稍后自动重试";
    case 503: return L"503：数据源暂时不可用，稍后自动重试";
    default: break;
    }
    wchar_t buf[64] = {};
    swprintf(buf, 64, L"HTTP %u", (unsigned)status);
    return buf;
}

std::wstring CodeText(const wchar_t* what, DWORD code)
{
    return std::wstring(what) + L" " + WinHttpErrorText(code);
}

} // namespace

bool HttpGet(const std::wstring& url, std::string& body, std::wstring& err)
{
    body.clear();
    err.clear();

    // 动态分配 host/path 缓冲，长 URL（比如自建内网签名地址）也不会被 2 KB 截断
    std::vector<wchar_t> hostBuf(url.size() + 1, L'\0');
    std::vector<wchar_t> pathBuf(url.size() + 1, L'\0');

    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = hostBuf.data();
    uc.dwHostNameLength = (DWORD)hostBuf.size();
    uc.lpszUrlPath = pathBuf.data();
    uc.dwUrlPathLength = (DWORD)pathBuf.size();
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        err = L"URL 解析失败";
        return false;
    }
    if (uc.nScheme != INTERNET_SCHEME_HTTP && uc.nScheme != INTERNET_SCHEME_HTTPS) {
        err = L"只支持 http/https 数据源";
        return false;
    }

    std::wstring host(hostBuf.data(), uc.dwHostNameLength);
    std::wstring path(pathBuf.data(), uc.dwUrlPathLength);
    if (path.empty()) path = L"/";
    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    std::wstring ua = L"ip-float/" IPFLOAT_VERSION_W;
    WinHttpHandle session(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        // Win7 / 早期 Win8 没有 AUTOMATIC_PROXY；退回系统默认代理设置
        session = WinHttpHandle(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    }
    if (!session) {
        err = CodeText(L"WinHttpOpen 失败", GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session.get(), kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);

    WinHttpHandle conn(WinHttpConnect(session.get(), host.c_str(), uc.nPort, 0));
    if (!conn) {
        err = CodeText(L"连接失败", GetLastError());
        return false;
    }

    WinHttpHandle req(WinHttpOpenRequest(conn.get(), L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         https ? WINHTTP_FLAG_SECURE : 0));
    if (!req) {
        err = CodeText(L"请求创建失败", GetLastError());
        return false;
    }

    WinHttpAddRequestHeaders(req.get(), L"Accept: application/json", (ULONG)-1L,
                             WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(req.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        err = CodeText(L"请求发送失败", GetLastError());
        return false;
    }
    if (!WinHttpReceiveResponse(req.get(), nullptr)) {
        err = CodeText(L"接收响应失败", GetLastError());
        return false;
    }

    DWORD status = 0;
    DWORD len = sizeof(status);
    if (!WinHttpQueryHeaders(req.get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                             WINHTTP_NO_HEADER_INDEX)) {
        err = CodeText(L"读取状态码失败", GetLastError());
        return false;
    }
    if (status != 200) {
        err = HttpStatusText(status);
        return false;
    }

    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.get(), &avail)) {
            err = CodeText(L"读取响应失败", GetLastError());
            return false;
        }
        if (avail == 0) break;
        if (body.size() + (size_t)avail > kMaxBody) {
            err = L"响应过大（超过 1 MiB）";
            return false;
        }

        std::string chunk((size_t)avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req.get(), chunk.data(), avail, &read) || read == 0) {
            err = CodeText(L"读取响应失败", GetLastError());
            return false;
        }
        chunk.resize((size_t)read);
        body += chunk;
    }

    if (body.empty()) {
        err = L"响应为空";
        return false;
    }
    return true;
}
