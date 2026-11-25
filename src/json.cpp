#include "json.h"

#include <climits>
#include <cstddef>
#include <string>

namespace {

void SkipWs(const std::string& j, size_t& i, size_t end)
{
    while (i < end && (j[i] == ' ' || j[i] == '\t' || j[i] == '\n' || j[i] == '\r'))
        ++i;
}

// i 指向开引号；成功后 i 指向闭引号之后
bool SkipString(const std::string& j, size_t& i, size_t end)
{
    if (i >= end || j[i] != '"') return false;
    ++i;
    while (i < end) {
        const char c = j[i++];
        if (c == '\\') {
            if (i < end) ++i;      // 转义的下一个字节可能是引号/反斜杠
        } else if (c == '"') {
            return true;
        }
    }
    return false;
}

// open 指向 '{'，close 返回与之配对的 '}'。字符串里的括号不参与计数。
bool MatchObject(const std::string& j, size_t open, size_t end, size_t& close)
{
    if (open >= end || j[open] != '{') return false;
    int depth = 0;
    size_t i = open;
    while (i < end) {
        const char c = j[i];
        if (c == '"') {
            if (!SkipString(j, i, end)) return false;
            continue;
        }
        if (c == '{' || c == '[') {
            ++depth;
        } else if (c == '}' || c == ']') {
            --depth;
            if (depth == 0) {
                if (c != '}') return false;
                close = i;
                return true;
            }
            if (depth < 0) return false;
        }
        ++i;
    }
    return false;
}

// 在 [begin,end) 里找第一个 "key":，valuePos 指向冒号之后。
// 不限制深度：简单的 "asn" 也能读到 ipwho.is 的 connection.asn；
// 需要精确时用点号路径先把范围缩到父对象。
bool FindKey(const std::string& j, size_t begin, size_t end,
             const std::string& key, size_t& valuePos)
{
    size_t i = begin;
    while (i < end) {
        if (j[i] != '"') {
            ++i;
            continue;
        }
        const size_t keyStart = i;
        if (!SkipString(j, i, end)) return false;
        const std::string got = j.substr(keyStart + 1, i - keyStart - 2);
        size_t p = i;
        SkipWs(j, p, end);
        if (p >= end || j[p] != ':') continue;
        if (got == key) {
            valuePos = p + 1;
            return true;
        }
        i = p + 1;
    }
    return false;
}

bool ResolveValue(const std::string& json, const std::string& path, size_t& valuePos)
{
    if (path.empty()) return false;
    size_t begin = 0, end = json.size();
    size_t segStart = 0;

    for (;;) {
        const size_t dot = path.find('.', segStart);
        const std::string key = path.substr(segStart,
                                            dot == std::string::npos ? std::string::npos
                                                                     : dot - segStart);
        if (key.empty()) return false;

        size_t vp = 0;
        if (!FindKey(json, begin, end, key, vp)) return false;
        if (dot == std::string::npos) {
            valuePos = vp;
            return true;
        }

        size_t p = vp;
        SkipWs(json, p, end);
        if (p >= end || json[p] != '{') return false;
        size_t close = 0;
        if (!MatchObject(json, p, end, close)) return false;
        begin = p + 1;
        end = close;
        segStart = dot + 1;
    }
}

bool JsonHex4(const std::string& j, size_t i, unsigned& out)
{
    out = 0;
    if (i + 5 >= j.size()) return false;
    for (int k = 2; k < 6; ++k) {
        const char h = j[i + (size_t)k];
        unsigned d = 0;
        if (h >= '0' && h <= '9')      d = (unsigned)(h - '0');
        else if (h >= 'a' && h <= 'f') d = (unsigned)(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') d = (unsigned)(h - 'A' + 10);
        else return false;
        out = out * 16 + d;
    }
    return true;
}

void AppendUtf8(std::string& out, unsigned cp)
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

// 解析 "..."，i 指向开引号；成功后 i 指向闭引号之后
bool ParseString(const std::string& j, size_t& i, size_t end, std::string& out)
{
    out.clear();
    if (i >= end || j[i] != '"') return false;
    ++i;
    while (i < end && j[i] != '"') {
        if (j[i] != '\\') {
            out += j[i++];
            continue;
        }
        if (i + 1 >= end) { out += '\\'; ++i; continue; }
        const char c = j[i + 1];
        if (c == 'n') { out += '\n'; i += 2; }
        else if (c == 't') { out += '\t'; i += 2; }
        else if (c == 'r') { out += '\r'; i += 2; }
        else if (c == 'b') { out += '\b'; i += 2; }
        else if (c == 'f') { out += '\f'; i += 2; }
        else if (c == 'u') {
            if (i + 5 >= end) {
                // 截断的 \u：保留 "\u"，跳到 u 之后继续按普通字符处理
                out += '\\';
                out += 'u';
                i += 2;
                continue;
            }
            unsigned code = 0;
            if (!JsonHex4(j, i, code)) {
                // 非法 \uXXXX：保留 "\u"，后面的字符按普通文本继续走
                out += '\\';
                out += 'u';
                i += 2;
                continue;
            }
            i += 6;      // 跳过 \uXXXX
            if (code >= 0xD800u && code <= 0xDBFFu &&
                i + 5 < end && j[i] == '\\' && j[i + 1] == 'u') {
                unsigned lo = 0;
                if (JsonHex4(j, i, lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                    code = 0x10000u + ((code - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 6;
                }
            }
            if (code >= 0xD800u && code <= 0xDFFFu) code = 0xFFFDu;
            AppendUtf8(out, code);
        } else {
            out += c;
            i += 2;
        }
    }
    if (i < end && j[i] == '"') ++i;
    return true;
}

bool RawValue(const std::string& j, const std::string& path, size_t& pos, size_t& end)
{
    if (!ResolveValue(j, path, pos)) return false;
    end = j.size();
    SkipWs(j, pos, end);
    return pos < end;
}

} // namespace

std::string JsonStr(const std::string& json, const std::string& path)
{
    size_t pos = 0, end = 0;
    if (!RawValue(json, path, pos, end)) return std::string();
    if (json[pos] != '"') return std::string();
    std::string out;
    if (!ParseString(json, pos, end, out)) return std::string();
    return out;
}

long long JsonInt(const std::string& json, const std::string& path, bool& found)
{
    found = false;
    size_t pos = 0, end = 0;
    if (!RawValue(json, path, pos, end)) return 0;

    bool quoted = false;
    if (json[pos] == '"') {
        quoted = true;
        ++pos;
        SkipWs(json, pos, end);
    }

    bool neg = false;
    if (pos < end && json[pos] == '-') { neg = true; ++pos; }
    if (pos >= end || json[pos] < '0' || json[pos] > '9') return 0;

    long long v = 0;
    while (pos < end && json[pos] >= '0' && json[pos] <= '9') {
        const int d = json[pos] - '0';
        if (v > (LLONG_MAX - d) / 10)   // 恶意/异常大数：当作没找到，避免有符号溢出
            return 0;
        v = v * 10 + d;
        ++pos;
    }
    if (quoted && (pos >= end || json[pos] != '"')) return 0;

    found = true;
    return neg ? -v : v;
}

bool JsonBool(const std::string& json, const std::string& path, bool& found)
{
    found = false;
    size_t pos = 0, end = 0;
    if (!RawValue(json, path, pos, end)) return false;
    if (json.compare(pos, 4, "true") == 0) { found = true; return true; }
    if (json.compare(pos, 5, "false") == 0) { found = true; return false; }
    return false;
}
