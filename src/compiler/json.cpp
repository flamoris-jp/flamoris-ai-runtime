#include "flamoris/runtime/compiler.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <stdexcept>
namespace flamoris::runtime {
namespace {
using Json = nlohmann::json;
constexpr double numeric_max = 9007199254740991.0;
struct Invalid {};
bool integral_token(std::string_view token) {
    if (token.starts_with('-'))
        token.remove_prefix(1);
    const auto e = token.find_first_of("eE");
    const auto significand = token.substr(0, e);
    if (significand.find_first_of("123456789") == std::string_view::npos)
        return true;
    std::int64_t exponent = 0;
    if (e != std::string_view::npos) {
        auto exp = token.substr(e + 1);
        bool negative = exp.starts_with('-');
        if (exp.starts_with('+') || negative)
            exp.remove_prefix(1);
        for (char c : exp) {
            if (exponent > 2000000)
                break;
            exponent = exponent * 10 + (c - '0');
        }
        if (negative)
            exponent = -exponent;
    }
    const auto point = significand.find('.');
    const std::int64_t fractional = point == std::string_view::npos
                                        ? 0
                                        : static_cast<std::int64_t>(significand.size() - point - 1);
    std::int64_t trailing = 0;
    for (auto it = significand.rbegin(); it != significand.rend(); ++it) {
        if (*it == '.')
            continue;
        if (*it != '0')
            break;
        ++trailing;
    }
    return exponent >= fractional - trailing;
}
struct BoundedSax final : nlohmann::json_sax<Json> {
    struct Frame {
        JsonValue value;
        std::optional<std::string> key;
    };
    JsonBounds bounds;
    std::vector<Frame> frames;
    std::optional<JsonValue> root;
    std::size_t scalars{0}, values{0};
    explicit BoundedSax(JsonBounds b) : bounds(b) {}
    bool scalar(std::size_t n) {
        if (n > bounds.max_scalar_bytes - scalars)
            return false;
        scalars += n;
        return true;
    }
    bool put(JsonValue value) {
        if (++values > bounds.max_values)
            return false;
        if (frames.empty()) {
            if (root)
                return false;
            root = std::move(value);
            return true;
        }
        auto &frame = frames.back();
        if (auto *a = std::get_if<JsonValue::Array>(&frame.value.data))
            a->push_back(std::move(value));
        else {
            if (!frame.key)
                return false;
            std::get<JsonValue::Object>(frame.value.data)
                .emplace(std::move(*frame.key), std::move(value));
            frame.key.reset();
        }
        return true;
    }
    bool null() override { return scalar(4) && put(nullptr); }
    bool boolean(bool v) override { return scalar(5) && put(v); }
    bool number_integer(number_integer_t v) override {
        return v >= -9007199254740991LL && v <= 9007199254740991LL && scalar(8) &&
               put(static_cast<double>(v));
    }
    bool number_unsigned(number_unsigned_t v) override {
        return v <= 9007199254740991ULL && scalar(8) && put(static_cast<double>(v));
    }
    bool number_float(number_float_t v, const string_t &token) override {
        if (!std::isfinite(v) || std::abs(v) > numeric_max)
            return false;
        if (v == 0) {
            const auto e = token.find_first_of("eE");
            const auto significand = token.substr(0, e);
            if (significand.find_first_of("123456789") != std::string::npos)
                return false;
        }
        JsonValue number(v == 0 ? 0.0 : v);
        number.exact_integer = integral_token(token);
        return scalar(8) && put(std::move(number));
    }
    bool string(string_t &v) override {
        return v.size() <= bounds.max_string_bytes && scalar(v.size()) && put(std::move(v));
    }
    bool binary(binary_t &) override { return false; }
    bool start_object(std::size_t) override {
        if (frames.size() >= bounds.max_depth)
            return false;
        frames.push_back({JsonValue::Object{}, {}});
        return true;
    }
    bool key(string_t &key) override {
        if (frames.empty() || key.size() > bounds.max_key_bytes || !scalar(key.size()))
            return false;
        auto &f = frames.back();
        if (f.key || std::get<JsonValue::Object>(f.value.data).contains(key))
            return false;
        f.key = std::move(key);
        return true;
    }
    bool end_object() override {
        if (frames.empty() || frames.back().key)
            return false;
        auto value = std::move(frames.back().value);
        frames.pop_back();
        return put(std::move(value));
    }
    bool start_array(std::size_t) override {
        if (frames.size() >= bounds.max_depth)
            return false;
        frames.push_back({JsonValue::Array{}, {}});
        return true;
    }
    bool end_array() override { return end_object(); }
    bool parse_error(std::size_t, const std::string &,
                     const nlohmann::detail::exception &) override {
        return false;
    }
};
std::vector<std::uint16_t> utf16(std::string_view text) {
    std::vector<std::uint16_t> out;
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i++]);
        std::uint32_t cp = c;
        unsigned n = 0;
        if (c >= 0xc2 && c <= 0xdf) {
            cp = c & 31U;
            n = 1;
        } else if (c >= 0xe0 && c <= 0xef) {
            cp = c & 15U;
            n = 2;
        } else if (c >= 0xf0 && c <= 0xf4) {
            cp = c & 7U;
            n = 3;
        } else if (c >= 0x80)
            throw Invalid{};
        for (unsigned j = 0; j < n; ++j) {
            if (i >= text.size())
                throw Invalid{};
            auto b = static_cast<unsigned char>(text[i++]);
            if ((b & 0xc0U) != 0x80U)
                throw Invalid{};
            cp = (cp << 6U) | (b & 63U);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            throw Invalid{};
        if (cp < 0x10000)
            out.push_back(static_cast<std::uint16_t>(cp));
        else {
            cp -= 0x10000;
            out.push_back(static_cast<std::uint16_t>(0xd800 + (cp >> 10)));
            out.push_back(static_cast<std::uint16_t>(0xdc00 + (cp & 1023)));
        }
    }
    return out;
}
void quote(std::string &out, std::string_view s) {
    if (s.size() > 262144)
        throw Invalid{};
    (void)utf16(s);
    out += '"';
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            if (c < 32) {
                out += "\\u00";
                out += hex[c >> 4];
                out += hex[c & 15];
            } else
                out += static_cast<char>(c);
        }
    }
    out += '"';
}
std::string number(double v) {
    if (!std::isfinite(v) || std::abs(v) > numeric_max)
        throw Invalid{};
    if (v == 0)
        return "0";
    char buf[64];
    auto [end, ec] = std::to_chars(buf, buf + 64, v, std::chars_format::general);
    if (ec != std::errc{})
        throw Invalid{};
    std::string s(buf, end);
    const bool neg = s[0] == '-';
    if (neg)
        s.erase(0, 1);
    auto epos = s.find_first_of("eE");
    int exponent = 0;
    if (epos != std::string::npos) {
        auto exp = s.substr(epos + 1);
        if (exp[0] == '+')
            exp.erase(0, 1);
        auto [p, err] = std::from_chars(exp.data(), exp.data() + exp.size(), exponent);
        if (err != std::errc{} || p != exp.data() + exp.size())
            throw Invalid{};
        s.resize(epos);
    }
    const auto point = s.find('.');
    int decimal = point == std::string::npos ? static_cast<int>(s.size()) : static_cast<int>(point);
    if (point != std::string::npos)
        s.erase(point, 1);
    decimal += exponent;
    while (s.size() > 1 && s.front() == '0') {
        s.erase(0, 1);
        --decimal;
    }
    while (s.size() > 1 && s.back() == '0')
        s.pop_back();
    std::string result;
    if (decimal > 0 && decimal <= 21) {
        result = s;
        if (static_cast<int>(s.size()) < decimal)
            result.append(static_cast<std::size_t>(decimal) - s.size(), '0');
        else if (static_cast<int>(s.size()) > decimal)
            result.insert(static_cast<std::size_t>(decimal), 1, '.');
    } else if (decimal <= 0 && decimal > -6) {
        result = "0.";
        result.append(static_cast<std::size_t>(-decimal), '0');
        result += s;
    } else {
        result = s.substr(0, 1);
        if (s.size() > 1)
            result += '.' + s.substr(1);
        int exp = decimal - 1;
        result += 'e';
        if (exp >= 0)
            result += '+';
        result += std::to_string(exp);
    }
    return neg ? '-' + result : result;
}
void emit(std::string &out, const JsonValue &v, std::size_t depth, std::size_t &nodes) {
    if (depth > 32 || ++nodes > 65536 || out.size() > 10485760)
        throw Invalid{};
    std::visit(
        [&](const auto &value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::nullptr_t>)
                out += "null";
            else if constexpr (std::is_same_v<T, bool>)
                out += value ? "true" : "false";
            else if constexpr (std::is_same_v<T, double>)
                out += number(value);
            else if constexpr (std::is_same_v<T, std::string>)
                quote(out, value);
            else if constexpr (std::is_same_v<T, JsonValue::Array>) {
                if (value.size() > 65536)
                    throw Invalid{};
                out += '[';
                bool first = true;
                for (const auto &item : value) {
                    if (!first)
                        out += ',';
                    first = false;
                    emit(out, item, depth + 1, nodes);
                }
                out += ']';
            } else {
                std::vector<
                    std::pair<std::vector<std::uint16_t>, const JsonValue::Object::value_type *>>
                    keys;
                if (value.size() > 65536)
                    throw Invalid{};
                for (const auto &entry : value) {
                    if (entry.first.size() > 256)
                        throw Invalid{};
                    keys.emplace_back(utf16(entry.first), &entry);
                }
                std::sort(keys.begin(), keys.end(),
                          [](const auto &a, const auto &b) { return a.first < b.first; });
                out += '{';
                bool first = true;
                for (const auto &entry : keys) {
                    if (!first)
                        out += ',';
                    first = false;
                    quote(out, entry.second->first);
                    out += ':';
                    emit(out, entry.second->second, depth + 1, nodes);
                }
                out += '}';
            }
        },
        v.data);
}
} // namespace
Result<JsonValue> parse_bounded_json_value(std::string_view text, JsonBounds bounds) {
    try {
        if (text.size() > bounds.max_bytes || text.size() > 1048576 ||
            text.starts_with("\xef\xbb\xbf") || bounds.max_depth > 32 ||
            bounds.max_string_bytes > 262144 || bounds.max_scalar_bytes > 1048576 ||
            bounds.max_key_bytes > 256 || bounds.max_values > 65536)
            throw Invalid{};
        BoundedSax sax(bounds);
        if (!Json::sax_parse(text.begin(), text.end(), &sax, Json::input_format_t::json, true,
                             false) ||
            !sax.root)
            throw Invalid{};
        return Result<JsonValue>::success(std::move(*sax.root));
    } catch (...) {
        return Result<JsonValue>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
    }
}
Result<JsonValue> parse_bounded_json(std::string_view text, JsonBounds bounds) {
    auto value = parse_bounded_json_value(text, bounds);
    if (!value)
        return value;
    if (!std::holds_alternative<JsonValue::Object>(value.value().data))
        return Result<JsonValue>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
    return value;
}
Result<std::string> canonical_json(const JsonValue &value) {
    try {
        std::string out;
        std::size_t nodes = 0;
        emit(out, value, 0, nodes);
        if (out.size() > 10485760)
            throw Invalid{};
        return Result<std::string>::success(std::move(out));
    } catch (...) {
        return Result<std::string>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
    }
}
Result<std::string> domain_digest(std::string_view domain, const JsonValue &value) {
    auto canonical = canonical_json(value);
    if (!canonical)
        return Result<std::string>::failure(canonical.error());
    try {
        std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(),
                                                                    &EVP_MD_CTX_free);
        std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
        unsigned int size = 0;
        if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1 ||
            EVP_DigestUpdate(ctx.get(), domain.data(), domain.size()) != 1 ||
            EVP_DigestUpdate(ctx.get(), canonical.value().data(), canonical.value().size()) != 1 ||
            EVP_DigestFinal_ex(ctx.get(), bytes.data(), &size) != 1 || size != 32)
            return Result<std::string>::failure(ErrorEnvelope::make(ErrorCode::internal_error));
        constexpr char hex[] = "0123456789abcdef";
        std::string digest;
        digest.reserve(64);
        for (unsigned i = 0; i < size; ++i) {
            digest += hex[bytes[i] >> 4];
            digest += hex[bytes[i] & 15];
        }
        return Result<std::string>::success(std::move(digest));
    } catch (...) {
        return Result<std::string>::failure(ErrorEnvelope::make(ErrorCode::internal_error));
    }
}
} // namespace flamoris::runtime
