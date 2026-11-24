#include "ipinfo.h"

#include "util.h"

#include <cwchar>
#include <cwctype>

// ── JSON 字符串读取（够用就好，不追求通用 JSON 解析器）─────────────────────

// 读 \uXXXX（i 指向反斜杠）→ 码点；不是合法四位十六进制就返回 false
static bool JsonHex4(const std::string& json, size_t i, unsigned& out)
{
    out = 0;
    if (i + 5 >= json.size()) return false;
    for (int k = 2; k < 6; ++k) {
        char h = json[i + (size_t)k];
        unsigned d;
        if (h >= '0' && h <= '9')      d = (unsigned)(h - '0');
        else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
        else return false;
        out = out * 16 + d;
    }
    return true;
}

// 码点 → UTF-8；正确处理 BMP 之外的四字节序列
static void AppendUtf8(std::string& out, unsigned cp)
{
    if (cp < 0x80) {
        out += (char)cp;
    } else if (cp < 0x800) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

std::string JsonStr(const std::string& json, const std::string& key)
{
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    while (pos != std::string::npos) {
        size_t i = pos + pat.size();
        while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
        if (i < json.size() && json[i] == ':') {
            ++i;
            while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
            if (i < json.size() && json[i] == '"') {
                ++i;
                std::string out;
                while (i < json.size() && json[i] != '"') {
                    if (json[i] == '\\' && i + 1 < json.size()) {
                        char c = json[i + 1];
                        // 注意：简单转义要把 i 推进到转义字符本身，
                        // 循环末尾的统一 ++i 再推到下一个字符；否则会把 "n" 原样留下
                        if (c == 'n') { out += '\n'; ++i; }
                        else if (c == 't') { out += '\t'; ++i; }
                        else if (c == 'r') { out += '\r'; ++i; }
                        else if (c == 'b') { out += '\b'; ++i; }
                        else if (c == 'f') { out += '\f'; ++i; }
                        else if (c == 'u') {
                            if (i + 5 >= json.size()) {
                                // 截断的 \u，原样保留
                                out += '\\';
                                out += 'u';
                                ++i;
                            } else {
                                unsigned code = 0;
                                if (!JsonHex4(json, i, code)) {
                                    // 非法 \uXXXX：保留 "\u"，后面的字符按普通文本继续走，
                                    // 这样截断的 "\u12" 也不会把引号/花括号吞进结果
                                    out += '\\';
                                    out += 'u';
                                    ++i;
                                } else {
                                    i += 5;                   // i 停在最后一个十六进制位
                                    // 高代理 + 低代理 → 合成 BMP 之外的码点
                                    if (code >= 0xD800u && code <= 0xDBFFu &&
                                        i + 6 < json.size() &&
                                        json[i + 1] == '\\' && json[i + 2] == 'u') {
                                        unsigned lo = 0;
                                        if (JsonHex4(json, i + 1, lo) &&
                                            lo >= 0xDC00u && lo <= 0xDFFFu) {
                                            code = 0x10000u + ((code - 0xD800u) << 10)
                                                 + (lo - 0xDC00u);
                                            i += 6;
                                        }
                                    }
                                    // 落单的代理项还原不了，按 Unicode 惯例写 U+FFFD
                                    if (code >= 0xD800u && code <= 0xDFFFu) code = 0xFFFDu;
                                    AppendUtf8(out, code);
                                }
                            }
                        } else {
                            out += c;
                            ++i;
                        }
                        ++i;
                    } else {
                        out += json[i];
                        ++i;
                    }
                }
                return out;
            }
            return "";
        }
        pos = json.find(pat, pos + 1);
    }
    return "";
}

// ── 解析 ────────────────────────────────────────────────────────────────────

bool ParseIpInfo(const std::string& json, const std::wstring& source, IpInfo& out,
                 std::wstring& err)
{
    out = IpInfo();
    out.ip = Utf8ToWide(JsonStr(json, "ip"));
    if (out.ip.empty()) {
        err = L"响应里没有 ip 字段";
        return false;
    }

    std::string country = JsonStr(json, "country_code");
    if (country.empty()) country = JsonStr(json, "country");
    out.country = Utf8ToWide(country);
    out.region  = Utf8ToWide(JsonStr(json, "region"));
    out.city    = Utf8ToWide(JsonStr(json, "city"));
    out.utcOffsetMinutes = ParseUtcOffset(JsonStr(json, "utc_offset"));
    std::wstring org = Utf8ToWide(JsonStr(json, "org"));
    out.asn = Utf8ToWide(JsonStr(json, "asn"));

    // ipinfo.io 把 ASN 塞在 org 里："AS20473 The Constant Company, LLC"
    if (org.size() > 3 && (org[0] == L'A' || org[0] == L'a') &&
        (org[1] == L'S' || org[1] == L's')) {
        size_t sp = org.find(L' ');
        std::wstring head = org.substr(0, sp == std::wstring::npos ? org.size() : sp);
        bool digits = head.size() > 2;
        for (size_t i = 2; i < head.size() && digits; ++i)
            if (head[i] < L'0' || head[i] > L'9') digits = false;
        if (digits) {
            out.asn = head;
            out.org = (sp == std::wstring::npos) ? L"" : org.substr(sp + 1);
            while (!out.org.empty() && out.org[0] == L' ') out.org.erase(0, 1);
        } else {
            out.org = org;
        }
    } else {
        out.org = org;
    }

    // 统一成 "AS" + 大写数字；ipapi 有时只给 "20473"
    if (!out.asn.empty()) {
        std::wstring a = out.asn;
        if (a.size() > 1 && (a[0] == L'a' || a[0] == L'A') &&
            (a[1] == L's' || a[1] == L'S')) {
            a = L"AS" + a.substr(2);
        } else if (a[0] != L'A' && a[0] != L'a') {
            a = L"AS" + a;
        }
        for (wchar_t& ch : a) ch = (wchar_t)towupper(ch);
        out.asn = a;
    }

    out.source = source;
    out.at = NowFileTime();
    return true;
}

std::wstring ShortAsn(const IpInfo& info){
    if (info.asn.empty() && info.org.empty()) return L"";

    struct Alias { const wchar_t* pattern; const wchar_t* name; };
    static const Alias aliases[] = {
        { L"constant|choopa|vultr",       L"Vultr"   },
        { L"digitalocean",                L"DO"      },
        { L"amazon|aws",                  L"AWS"     },
        { L"google",                      L"Google"  },
        { L"microsoft|azure",             L"Azure"   },
        { L"cloudflare",                  L"CF"      },
        { L"oracle",                      L"Oracle"  },
        { L"linode|akamai",               L"Linode"  },
        { L"bandwagon|it7",               L"BWG"     },
        { L"alibaba|aliyun",              L"Aliyun"  },
        { L"tencent",                     L"Tencent" },
        { L"china telecom|chinatelecom",  L"CT"      },
        { L"china unicom|chinaunicom",    L"CU"      },
    };

    std::wstring lower;
    lower.reserve(info.org.size());
    for (wchar_t ch : info.org) lower += (wchar_t)towlower(ch);

    std::wstring shown = info.org;
    for (const Alias& a : aliases) {
        // 逐个候选词做子串匹配，避免引入正则
        const wchar_t* p = a.pattern;
        while (*p) {
            const wchar_t* bar = wcschr(p, L'|');
            size_t len = bar ? (size_t)(bar - p) : wcslen(p);
            if (len > 0 && lower.find(std::wstring(p, len)) != std::wstring::npos) {
                shown = a.name;
                break;
            }
            if (!bar) break;
            p = bar + 1;
        }
        if (shown != info.org) break;
    }

    if (shown == info.org && shown.size() > 14) {
        size_t comma = shown.find(L',');
        if (comma != std::wstring::npos) shown = shown.substr(0, comma);
        const wchar_t* tails[] = { L"LLC", L"Inc", L"Corp", L"Corporation",
                                   L"Ltd", L"Limited", L"LLP" };
        for (const wchar_t* tailWord : tails) {
            std::wstring tail = std::wstring(L" ") + tailWord;
            if (shown.size() > tail.size() &&
                _wcsicmp(shown.c_str() + shown.size() - tail.size(), tail.c_str()) == 0) {
                shown = shown.substr(0, shown.size() - tail.size());
            }
        }
        while (!shown.empty() &&
               (shown.back() == L' ' || shown.back() == L'.'))
            shown.pop_back();
        // 取前两个词
        size_t sp1 = shown.find(L' ');
        if (sp1 != std::wstring::npos) {
            size_t sp2 = shown.find(L' ', sp1 + 1);
            if (sp2 != std::wstring::npos) shown = shown.substr(0, sp2);
        }
        if (shown.size() > 14) shown = shown.substr(0, 14);
    }

    if (!info.asn.empty())
        return shown.empty() ? info.asn : info.asn + L" " + shown;
    return shown;
}

// "+0800" / "-0500" → 分钟；格式不对返回 kNoOffset
int ParseUtcOffset(const std::string& s)
{
    if (s.size() != 5) return kNoOffset;
    if (s[0] != '+' && s[0] != '-') return kNoOffset;
    for (int i = 1; i < 5; ++i)
        if (s[i] < '0' || s[i] > '9') return kNoOffset;
    int hh = (s[1] - '0') * 10 + (s[2] - '0');
    int mm = (s[3] - '0') * 10 + (s[4] - '0');
    if (hh > 14 || mm > 59) return kNoOffset;
    int v = hh * 60 + mm;
    return s[0] == '-' ? -v : v;
}
