#pragma once

#include <string>

// 极简 JSON 取值工具（不是完整解析器）。只做一件事：按字段名取出原始值。
//
// - 路径用点号访问嵌套对象：JsonStr(json, "connection.asn")
// - 找不到 / 类型不对时返回空串或 0（JsonInt 用 found 区分 0 与没找到）
// - 字符串会处理常见转义与 \uXXXX；不支持数组下标，也不追求通用 JSON 兼容
//
// 单独拆出来是为了让 ipinfo（解析数据源）和 diag（解析 ipwho.is）共用一套规则，
// 避免同一份 JSON 在两个文件里被两种方式解释。
std::string JsonStr(const std::string& json, const std::string& path);
long long   JsonInt(const std::string& json, const std::string& path, bool& found);
bool        JsonBool(const std::string& json, const std::string& path, bool& found);
