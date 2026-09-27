#include "flamoris/runtime/effects.hpp"
#include <array>

namespace flamoris::runtime {
namespace {
constexpr std::array<std::string_view, 6> effect_names{"pure",     "read",        "write",
                                                       "external", "destructive", "paid"};
Result<EffectSet> invalid_effects() noexcept {
    return Result<EffectSet>::failure(ErrorEnvelope::make(
        ErrorCode::invalid_workflow, ErrorStage::validation, ExternalOutcome::not_applicable,
        RetryDisposition::prohibited, ErrorReason::invalid_effects));
}
} // namespace
Result<EffectSet> EffectSet::from_mask(std::uint8_t mask) noexcept {
    if (mask == 0 || (mask & 0xc0U) != 0 || ((mask & 1U) != 0 && mask != 1) ||
        ((mask & 16U) != 0 && (mask & 4U) == 0))
        return invalid_effects();
    return Result<EffectSet>::success(EffectSet(mask));
}
Result<EffectSet> EffectSet::from_names(std::span<const std::string_view> input) noexcept {
    std::uint8_t mask{};
    for (const auto name : input) {
        bool found = false;
        for (std::size_t i = 0; i < effect_names.size(); ++i) {
            if (name != effect_names[i])
                continue;
            const auto bit = static_cast<std::uint8_t>(1U << i);
            if ((mask & bit) != 0)
                return invalid_effects();
            mask = static_cast<std::uint8_t>(mask | bit);
            found = true;
            break;
        }
        if (!found)
            return invalid_effects();
    }
    return from_mask(mask);
}
Result<EffectSet> EffectSet::aggregate(std::span<const EffectSet> members) noexcept {
    if (members.empty())
        return invalid_effects();
    std::uint8_t mask{};
    for (const auto member : members)
        mask = static_cast<std::uint8_t>(mask | member.mask());
    // Pure members contribute no ambient effect; a wholly pure aggregate stays pure.
    if (mask != 1)
        mask = static_cast<std::uint8_t>(mask & ~1U);
    return from_mask(mask);
}
std::vector<std::string_view> EffectSet::names() const {
    std::vector<std::string_view> result;
    for (std::size_t i = 0; i < effect_names.size(); ++i)
        if ((mask_ & (1U << i)) != 0)
            result.push_back(effect_names[i]);
    return result;
}
} // namespace flamoris::runtime
