#include "flamoris/runtime/limits.hpp"
#include <array>
#include <utility>

namespace flamoris::runtime {
// Every Result alternative is checked immediately before value()/error() access.
// NOLINTNEXTLINE(bugprone-exception-escape)
Result<std::uint64_t> mandatory_event_slots(const EventReservation &b) noexcept {
    auto update_coefficient = checked_multiply(b.post_terminal_updates, 2);
    if (!update_coefficient)
        return update_coefficient;
    auto cleanup_coefficient = checked_add(update_coefficient.value(), 4);
    if (!cleanup_coefficient)
        return cleanup_coefficient;
    const std::array<std::pair<std::uint64_t, std::uint64_t>, 8> terms{
        {{b.jobs, 16},
         {b.attempts, 12},
         {b.suspensions, 8},
         {b.commands, 8},
         {b.groups, 8},
         {b.resource_operations, 8},
         {b.dynamic_proposals, 8},
         {b.cleanup_records, cleanup_coefficient.value()}}};
    std::uint64_t total = 32;
    for (const auto &[count, coefficient] : terms) {
        auto product = checked_multiply(count, coefficient);
        if (!product)
            return product;
        auto sum = checked_add(total, product.value());
        if (!sum)
            return sum;
        total = sum.value();
    }
    return Result<std::uint64_t>::success(total);
}
// Every Result alternative is checked immediately before value()/error() access.
// NOLINTNEXTLINE(bugprone-exception-escape)
Result<std::uint64_t> mandatory_event_storage(const EventReservation &bounds,
                                              std::uint64_t ceiling) noexcept {
    auto slots = mandatory_event_slots(bounds);
    if (!slots)
        return slots;
    auto bytes = checked_multiply(slots.value(), 8 * 1024);
    if (!bytes)
        return bytes;
    if (bytes.value() > ceiling)
        return Result<std::uint64_t>::failure(ErrorEnvelope::make(
            ErrorCode::budget_exceeded, ErrorStage::admission, ExternalOutcome::not_applicable,
            RetryDisposition::prohibited, ErrorReason::limit_exceeded));
    return bytes;
}
} // namespace flamoris::runtime
