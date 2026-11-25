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
    case 12037: hint = L"证书已过期或尚未生效"; break;
    case 12038: hint = L"证书域名不匹配"; break;
    case 12045: hint = L"证书颁发机构不受信任"; break;
    case 12057: hint = L"证书吊销状态无法确认"; break;
    case 12157: hint = L"TLS 安全通道错误"; break;
    case 12169: hint = L"证书无效"; break;
    case 12170: hint = L"证书已被吊销"; break;
    case 12175: hint = L"TLS 安全通道失败（证书或协议不兼容）"; break;
    case 12179: hint = L"证书用途不匹配"; break;
    case 12185: hint = L"客户端证书缺少私钥（常见于受限账户或代理）"; break;
    case 12186: hint = L"客户端证书私钥不可访问"; break;
    case 12187: hint = L"代理要求客户端证书"; break;
    case 12188: hint = L"代理的 TLS 安全通道失败"; break;
    default: break;
    }
    wchar_t buf[160] = {};
    // 注意 %ls：MinGW 的 swprintf 是 C99 语义，%s 期待 char*，传 wchar_t* 会得到乱码
    if (hint) swprintf(buf, 160, L"%u：%ls", (unsigned)code, hint);
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

bool IsTlsCertErrorText(const std::wstring& err)
{
    // 这些码都是「证书本身不可信/不匹配」，lenient 标志能救回来；
    // 12185（客户端证书缺私钥）不属于其中，降级也没用。
    static const wchar_t* kCodes[] = {
        L"12037", L"12038", L"12045", L"12057",
        L"12169", L"12170", L"12175", L"12179",
    };
    for (const wchar_t* code : kCodes)
        if (err.find(code) != std::wstring::npos) return true;
    return false;
}

bool HttpGet(const std::wstring& url, std::string& body, std::wstring& err,
             int timeoutMs, bool direct, bool lenientTls)
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
    // IPv6 字面量：WinHttpCrackUrl 给的是 "[2606:4700:4700::1111]"，
    // WinHttpConnect 不认方括号，要去掉；留着会在 SendRequest 时 12029。
    if (host.size() >= 2 && host.front() == L'[' && host.back() == L']')
        host = host.substr(1, host.size() - 2);
    std::wstring path(pathBuf.data(), uc.dwUrlPathLength);
    if (path.empty()) path = L"/";
    const bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);

    std::wstring ua = L"ip-float/" IPFLOAT_VERSION_W;
    const DWORD accessType = direct ? WINHTTP_ACCESS_TYPE_NO_PROXY
                                    : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    WinHttpHandle session(WinHttpOpen(ua.c_str(), accessType,
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
    const DWORD tmo = timeoutMs > 0 ? (DWORD)timeoutMs : kTimeoutMs;
    WinHttpSetTimeouts(session.get(), tmo, tmo, tmo, tmo);

    // WinHTTP 默认会自己重试连接（3 次），于是「设 3 秒超时」实际能跑 8 秒。
    // 探测类请求靠这个参数控制总时长，隐式重试会让它彻底失真，所以关掉。
    DWORD noRetry = 0;
    WinHttpSetOption(session.get(), WINHTTP_OPTION_CONNECT_RETRIES, &noRetry, sizeof(noRetry));

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

    // 这些请求都不需要客户端证书。某些机器上证书库里会有一张没有私钥的
    // 客户端证书，WinHTTP 自动挑中它后会报 12185（CLIENT_CERT_NO_PRIVATE_KEY）。
    // 明确告诉 WinHTTP 不要找客户端证书。
    WinHttpSetOption(req.get(), WINHTTP_OPTION_CLIENT_CERT_CONTEXT,
                     (LPVOID)WINHTTP_NO_CLIENT_CERT_CONTEXT, 0);

    // 只给「公网 IP 回显」这类没有敏感数据的探针使用：某些代理会返回
    // 用途/CA 不匹配的证书，严格校验证书会导致永远拿不到 IP。
    if (lenientTls) {
        DWORD flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                      SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                      SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(req.get(), WINHTTP_OPTION_SECURITY_FLAGS, &flags, sizeof(flags));
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
