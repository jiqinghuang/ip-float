#include "ipinfo.h"

#include "util.h"

#include <cwchar>
#include <cwctype>

// ── 解析 ────────────────────────────────────────────────────────────────────

bool ParseIpInfo(const std::string& json, const std::wstring& source, IpInfo& out,
                 std::wstring& err)
{
    out = IpInfo();
    out.ip = Utf8ToWide(JsonStr(json, "ip"));

    // 常见源（ipinfo / ipapi / ipwho.is）的字段名不完全一样，这里按优先级取。
    // 空值/非对象（比如 ipinfo 的 timezone 是字符串）都会自然落到下一档。
    std::string country = JsonStr(json, "country_code");
    if (country.empty()) country = JsonStr(json, "country");
    out.country = Utf8ToWide(country);
    out.region  = Utf8ToWide(JsonStr(json, "region"));
    out.city    = Utf8ToWide(JsonStr(json, "city"));

    out.utcOffsetMinutes = ParseUtcOffset(JsonStr(json, "utc_offset"));
    if (out.utcOffsetMinutes == kNoOffset) {
        bool found = false;
        const long long seconds = JsonInt(json, "timezone.offset", found);
        if (found) out.utcOffsetMinutes = (int)(seconds / 60);
    }
    if (out.utcOffsetMinutes == kNoOffset)
        out.utcOffsetMinutes = ParseUtcOffset(JsonStr(json, "timezone.utc"));

    std::string asnText = JsonStr(json, "connection.asn");
    if (asnText.empty()) asnText = JsonStr(json, "asn");
    if (asnText.empty()) {
        bool found = false;
        long long n = JsonInt(json, "connection.asn", found);
        if (!found) n = JsonInt(json, "asn", found);
        if (found && n > 0) asnText = std::to_string(n);
    }
    out.asn = Utf8ToWide(asnText);

    std::wstring org = Utf8ToWide(JsonStr(json, "connection.org"));
    if (org.empty()) org = Utf8ToWide(JsonStr(json, "org"));
    if (org.empty()) org = Utf8ToWide(JsonStr(json, "isp"));

    // ipinfo.io 把 ASN 塞在 org 里："AS20473 The Constant Company, LLC"
    if (org.size() > 3 && (org[0] == L'A' || org[0] == L'a') &&
        (org[1] == L'S' || org[1] == L's')) {
        size_t sp = org.find(L' ');
        std::wstring head = org.substr(0, sp == std::wstring::npos ? org.size() : sp);
        bool digits = head.size() > 2;
        for (size_t i = 2; i < head.size() && digits; ++i)
            if (head[i] < L'0' || head[i] > L'9') digits = false;
        if (digits) {
            if (out.asn.empty()) out.asn = head;
            out.org = (sp == std::wstring::npos) ? L"" : org.substr(sp + 1);
            while (!out.org.empty() && out.org[0] == L' ') out.org.erase(0, 1);
        } else {
            out.org = org;
        }
    } else {
        out.org = org;
    }

    // 有些源会带首尾空白；地址族判定同时兼做合法性校验
    size_t a = 0, b = out.ip.size();
    while (a < b && iswspace(out.ip[a])) ++a;
    while (b > a && iswspace(out.ip[b - 1])) --b;
    out.ip = out.ip.substr(a, b - a);
    if (out.ip.empty()) {
        err = L"响应里没有 ip 字段";
        return false;
    }
    if (ClassifyIp(out.ip) == IpKind::None) {
        err = L"响应里的 ip 不是合法地址";
        return false;
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
    // 接受 "+0800"、"-0500"，也接受 ipwho.is 的 "+08:00"/"-05:00"
    if (s.size() != 5 && s.size() != 6) return kNoOffset;
    if (s[0] != '+' && s[0] != '-') return kNoOffset;
    const size_t colon = (s.size() == 6 && s[3] == ':') ? 3 : 0;
    if (s.size() == 6 && colon == 0) return kNoOffset;
    if (colon) {
        if (s[1] < '0' || s[1] > '9' || s[2] < '0' || s[2] > '9' ||
            s[4] < '0' || s[4] > '9' || s[5] < '0' || s[5] > '9')
            return kNoOffset;
    } else {
        for (int i = 1; i < 5; ++i)
            if (s[i] < '0' || s[i] > '9') return kNoOffset;
    }
    const int hh = (s[1] - '0') * 10 + (s[2] - '0');
    const int mm = colon ? ((s[4] - '0') * 10 + (s[5] - '0'))
                         : ((s[3] - '0') * 10 + (s[4] - '0'));
    if (hh > 14 || mm > 59) return kNoOffset;
    const int v = hh * 60 + mm;
    return s[0] == '-' ? -v : v;
}
