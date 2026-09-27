#include "flamoris/runtime/native.hpp"
#include <limits>

namespace flamoris::runtime {
namespace {
template<class T> Result<T> invalid() { return Result<T>::failure(ErrorEnvelope::make(ErrorCode::invalid_request)); }
}
Result<std::string> Utf8Decoder::append(std::string_view bytes, bool final) {
    if (bytes.size() > 1048576 || carry.size() > 3) return invalid<std::string>();
    std::string input = carry;
    input.append(bytes);
    std::size_t cursor = 0;
    while (cursor < input.size()) {
        auto lead = static_cast<unsigned char>(input[cursor]);
        std::size_t length = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
            lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        if (!length) return invalid<std::string>();
        auto present = std::min(length, input.size() - cursor);
        for (std::size_t j = 1; j < present; ++j) {
            auto byte = static_cast<unsigned char>(input[cursor + j]);
            if (byte < 0x80 || byte > 0xbf) return invalid<std::string>();
            if (j == 1 && ((lead == 0xe0 && byte < 0xa0) || (lead == 0xed && byte > 0x9f) ||
                (lead == 0xf0 && byte < 0x90) || (lead == 0xf4 && byte > 0x8f))) return invalid<std::string>();
        }
        if (present != length) break;
        cursor += length;
    }
    if (final && cursor != input.size()) return invalid<std::string>();
    auto output = input.substr(0, cursor);
    carry = input.substr(cursor);
    return Result<std::string>::success(std::move(output));
}
Result<void> validate_tokenizer(const TokenizerDefinition& definition) {
    if (definition != TokenizerDefinition{}) return invalid<void>();
    return Result<void>::success();
}
Result<std::vector<TokenId>> tokenize(std::string_view input, const TokenizerDefinition& definition,
    std::size_t max_bytes) {
    if (!validate_tokenizer(definition) || input.size() > max_bytes || max_bytes > 1048576) return invalid<std::vector<TokenId>>();
    Utf8Decoder validator;
    if (!validator.append(input, true)) return invalid<std::vector<TokenId>>();
    std::vector<TokenId> tokens;
    tokens.reserve(input.size());
    for (unsigned char c : input) tokens.push_back(c);
    return Result<std::vector<TokenId>>::success(std::move(tokens));
}
Result<std::string> detokenize(std::span<const TokenId> tokens, const TokenizerDefinition& definition) {
    if (!validate_tokenizer(definition) || tokens.size() > 1048576) return invalid<std::string>();
    std::string bytes;
    bytes.reserve(tokens.size());
    for (auto token : tokens) {
        if (token >= 256) return invalid<std::string>(); // Control tokens require explicit handling.
        bytes.push_back(static_cast<char>(token));
    }
    Utf8Decoder decoder;
    return decoder.append(bytes, true);
}
Result<std::string> utf16_to_utf8(std::u16string_view input, std::size_t max_bytes) {
    if (max_bytes > 1048576 || input.size() > max_bytes) return invalid<std::string>();
    std::string output;
    for (std::size_t i = 0; i < input.size(); ++i) {
        std::uint32_t scalar = input[i];
        if (scalar >= 0xd800 && scalar <= 0xdbff) {
            if (i + 1 == input.size() || input[i + 1] < 0xdc00 || input[i + 1] > 0xdfff) return invalid<std::string>();
            scalar = 0x10000 + ((scalar - 0xd800) << 10) + (input[++i] - 0xdc00);
        } else if (scalar >= 0xdc00 && scalar <= 0xdfff) return invalid<std::string>();
        if (scalar < 0x80) output.push_back(static_cast<char>(scalar));
        else if (scalar < 0x800) {
            output.push_back(static_cast<char>(0xc0 | (scalar >> 6)));
            output.push_back(static_cast<char>(0x80 | (scalar & 63)));
        } else if (scalar < 0x10000) {
            output.push_back(static_cast<char>(0xe0 | (scalar >> 12)));
            output.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63)));
            output.push_back(static_cast<char>(0x80 | (scalar & 63)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (scalar >> 18)));
            output.push_back(static_cast<char>(0x80 | ((scalar >> 12) & 63)));
            output.push_back(static_cast<char>(0x80 | ((scalar >> 6) & 63)));
            output.push_back(static_cast<char>(0x80 | (scalar & 63)));
        }
        if (output.size() > max_bytes) return invalid<std::string>();
    }
    return Result<std::string>::success(std::move(output));
}
} // namespace flamoris::runtime
