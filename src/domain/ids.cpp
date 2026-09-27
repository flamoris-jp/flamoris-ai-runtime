#include "flamoris/runtime/ids.hpp"

namespace flamoris::runtime {
Result<std::uint64_t> parse_counter(std::string_view text) noexcept {
    const auto invalid = [] { return Result<std::uint64_t>::failure(ErrorEnvelope::make(
        ErrorCode::invalid_request, ErrorStage::validation, ExternalOutcome::not_applicable,
        RetryDisposition::prohibited, ErrorReason::malformed_identifier)); };
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text.front() == '0')) return invalid();
    for (const char c : text) if (c < '0' || c > '9') return invalid();
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return invalid();
    return Result<std::uint64_t>::success(result);
}
} // namespace flamoris::runtime
