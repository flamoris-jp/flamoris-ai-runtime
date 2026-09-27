#pragma once
#include "flamoris/runtime/compiler.hpp"
#include <cmath>
#include <initializer_list>
namespace flamoris::runtime::compiler_detail {
struct Failure {
    ErrorCode code;
};
[[noreturn]] inline void reject(ErrorCode c = ErrorCode::invalid_workflow) { throw Failure{c}; }
inline const JsonValue::Object &object(const JsonValue &v) {
    auto p = std::get_if<JsonValue::Object>(&v.data);
    if (!p)
        reject();
    return *p;
}
inline const JsonValue::Array &array(const JsonValue &v) {
    auto p = std::get_if<JsonValue::Array>(&v.data);
    if (!p)
        reject();
    return *p;
}
inline const std::string &string(const JsonValue &v) {
    auto p = std::get_if<std::string>(&v.data);
    if (!p)
        reject();
    return *p;
}
inline double numeric(const JsonValue &v) {
    auto p = std::get_if<double>(&v.data);
    if (!p || !std::isfinite(*p) || std::abs(*p) > 9007199254740991.0)
        reject();
    return *p;
}
inline std::uint64_t count(const JsonValue &v, bool positive = false) {
    double x = numeric(v);
    if (x < 0 || std::floor(x) != x || (positive && x == 0))
        reject();
    return static_cast<std::uint64_t>(x);
}
inline const JsonValue &required(const JsonValue::Object &o, std::string_view key) {
    auto it = o.find(std::string(key));
    if (it == o.end())
        reject();
    return it->second;
}
inline void keys(const JsonValue::Object &o, std::initializer_list<std::string_view> required_keys,
                 std::initializer_list<std::string_view> optional = {}) {
    for (auto key : required_keys)
        if (!o.contains(std::string(key)))
            reject();
    for (const auto &[key, v] : o) {
        (void)v;
        bool found = false;
        for (auto k : required_keys)
            if (key == k)
                found = true;
        for (auto k : optional)
            if (key == k)
                found = true;
        if (!found)
            reject();
    }
}
inline bool identifier(std::string_view s) {
    if (s.empty() || s.size() > 128)
        return false;
    auto alpha = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
    };
    if (!alpha(s[0]))
        return false;
    for (char c : s)
        if (!alpha(c) && !(c >= '0' && c <= '9') && c != '.' && c != '-')
            return false;
    return true;
}
inline JsonValue integer(std::uint64_t n) {
    if (n > 9007199254740991ULL)
        reject();
    return JsonValue(static_cast<double>(n));
}
inline JsonValue::Array strings(const std::vector<std::string> &names) {
    JsonValue::Array result;
    for (const auto &n : names)
        result.emplace_back(n);
    return result;
}
ValueSchema schema(const JsonValue &, unsigned depth = 0);
const ValueSchema *path_schema(const ValueSchema &, const std::vector<PathComponent> &);
const JsonValue *path_value(const JsonValue &, const std::vector<PathComponent> &);
} // namespace flamoris::runtime::compiler_detail
