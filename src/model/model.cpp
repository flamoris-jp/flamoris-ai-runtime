#include "flamoris/runtime/native.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <openssl/evp.h>
#include <sstream>

namespace flamoris::runtime {
namespace {
template<class T> Result<T> fail(ErrorCode code = ErrorCode::unsupported_model) {
    return Result<T>::failure(ErrorEnvelope::make(code, ErrorStage::execution));
}
std::uint32_t little32(const unsigned char* bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
        (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}
}
Result<std::shared_ptr<const TinyModel>> TinyModel::load(const std::filesystem::path& path,
    std::string_view expected_sha256) {
    // Paths are trusted registration inputs, never resolved from Workflow/model output.
    constexpr std::size_t count = 258 * 8 + 256 * 8 + 5 * 8 * 8 + 258 * 8;
    constexpr std::size_t bytes = 20 + count * sizeof(float);
    if (expected_sha256.size() != 64) return fail<std::shared_ptr<const TinyModel>>();
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream || stream.tellg() != static_cast<std::streamoff>(bytes)) return fail<std::shared_ptr<const TinyModel>>();
    stream.seekg(0);
    std::vector<unsigned char> encoded(bytes);
    if (!stream.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(bytes)))
        return fail<std::shared_ptr<const TinyModel>>();
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned digest_size = 0;
    if (EVP_Digest(encoded.data(), encoded.size(), digest.data(), &digest_size, EVP_sha256(), nullptr) != 1 || digest_size != 32)
        return fail<std::shared_ptr<const TinyModel>>(ErrorCode::internal_error);
    constexpr char hex[] = "0123456789abcdef";
    std::string actual;
    actual.reserve(64);
    for (unsigned i = 0; i < digest_size; ++i) { actual.push_back(hex[digest[i] >> 4]); actual.push_back(hex[digest[i] & 15]); }
    if (actual != expected_sha256 || std::string_view(reinterpret_cast<const char*>(encoded.data()), 8) != "FTRTINY1" ||
        little32(encoded.data() + 8) != 8 || little32(encoded.data() + 12) != 258 || little32(encoded.data() + 16) != 256)
        return fail<std::shared_ptr<const TinyModel>>();
    auto model = std::shared_ptr<TinyModel>(new TinyModel);
    model->definition_.artifact_sha256 = actual;
    model->weights_.reserve(count);
    for (std::size_t offset = 20; offset < bytes; offset += 4) {
        auto value = std::bit_cast<float>(little32(encoded.data() + offset));
        if (!std::isfinite(value)) return fail<std::shared_ptr<const TinyModel>>();
        model->weights_.push_back(value);
    }
    return Result<std::shared_ptr<const TinyModel>>::success(std::move(model));
}
Result<std::vector<float>> TinyModel::evaluate(TokenId token, std::vector<float>& keys,
    std::vector<float>& values, ComputeImplementation& compute) const {
    constexpr std::size_t width = 8, vocab = 258, context = 256;
    if (token >= vocab || keys.size() != values.size() || keys.size() % width || keys.size() >= context * width)
        return fail<std::vector<float>>(ErrorCode::invalid_request);
    const std::size_t position = keys.size() / width;
    std::vector<float> hidden(width);
    for (std::size_t d = 0; d < width; ++d)
        hidden[d] = weights_[token * width + d] + weights_[vocab * width + position * width + d];
    std::size_t offset = vocab * width + context * width;
    auto projection = [&](std::span<const float> input, std::size_t rows) {
        auto result = compute.matvec(std::span<const float>(weights_).subspan(offset, rows * width), rows, width, input);
        offset += rows * width;
        return result;
    };
    auto query = projection(hidden, width);
    auto key = projection(hidden, width);
    auto value = projection(hidden, width);
    if (!query) return Result<std::vector<float>>::failure(query.error());
    if (!key) return Result<std::vector<float>>::failure(key.error());
    if (!value) return Result<std::vector<float>>::failure(value.error());
    keys.insert(keys.end(), key.value().begin(), key.value().end());
    values.insert(values.end(), value.value().begin(), value.value().end());
    auto attended = compute.attention(query.value(), keys, values, position + 1, width);
    if (!attended) return Result<std::vector<float>>::failure(attended.error());
    auto projected = projection(attended.value(), width);
    if (!projected) return Result<std::vector<float>>::failure(projected.error());
    for (std::size_t d = 0; d < width; ++d) hidden[d] += projected.value()[d];
    auto feed_forward = projection(hidden, width);
    if (!feed_forward) return Result<std::vector<float>>::failure(feed_forward.error());
    for (std::size_t d = 0; d < width; ++d) hidden[d] += std::tanh(feed_forward.value()[d]);
    return projection(hidden, vocab);
}
Result<std::vector<float>> TinyModel::uncached(std::span<const TokenId> tokens, ComputeImplementation& compute) const {
    if (tokens.empty() || tokens.size() > definition_.context) return fail<std::vector<float>>(ErrorCode::invalid_request);
    std::vector<float> keys, values, result;
    keys.reserve(tokens.size() * definition_.width);
    values.reserve(tokens.size() * definition_.width);
    for (auto token : tokens) {
        auto current = evaluate(token, keys, values, compute);
        if (!current) return current;
        result = std::move(current).value();
    }
    return Result<std::vector<float>>::success(std::move(result));
}
} // namespace flamoris::runtime
