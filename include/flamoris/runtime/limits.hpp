#pragma once

#include "flamoris/runtime/result.hpp"
#include <cstdint>
#include <limits>

namespace flamoris::runtime {

[[nodiscard]] inline Result<std::uint64_t> checked_add(std::uint64_t a, std::uint64_t b) noexcept {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        return Result<std::uint64_t>::failure(ErrorEnvelope::make(ErrorCode::budget_exceeded,
            ErrorStage::admission, ExternalOutcome::not_applicable, RetryDisposition::prohibited,
            ErrorReason::arithmetic_overflow));
    return Result<std::uint64_t>::success(a + b);
}
[[nodiscard]] inline Result<std::uint64_t> checked_multiply(std::uint64_t a, std::uint64_t b) noexcept {
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a)
        return Result<std::uint64_t>::failure(ErrorEnvelope::make(ErrorCode::budget_exceeded,
            ErrorStage::admission, ExternalOutcome::not_applicable, RetryDisposition::prohibited,
            ErrorReason::arithmetic_overflow));
    return Result<std::uint64_t>::success(a * b);
}
class FiniteLimit final {
public:
    [[nodiscard]] static Result<FiniteLimit> create(std::uint64_t value) noexcept {
        if (value == 0) return Result<FiniteLimit>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
        return Result<FiniteLimit>::success(FiniteLimit(value));
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
private:
    explicit FiniteLimit(std::uint64_t value) noexcept : value_(value) {}
    std::uint64_t value_;
};

class CheckedCounter final {
public:
    explicit CheckedCounter(std::uint64_t current = 0) noexcept : current_(current) {}
    [[nodiscard]] Result<std::uint64_t> next() noexcept {
        if (current_ == std::numeric_limits<std::uint64_t>::max())
            return Result<std::uint64_t>::failure(ErrorEnvelope::make(ErrorCode::budget_exceeded,
                ErrorStage::admission, ExternalOutcome::not_applicable, RetryDisposition::prohibited,
                ErrorReason::counter_exhausted));
        return Result<std::uint64_t>::success(++current_);
    }
    [[nodiscard]] std::uint64_t value() const noexcept { return current_; }
private:
    std::uint64_t current_;
};

struct EventReservation final {
    std::uint64_t jobs{};
    std::uint64_t attempts{};
    std::uint64_t suspensions{};
    std::uint64_t commands{};
    std::uint64_t groups{};
    std::uint64_t resource_operations{};
    std::uint64_t dynamic_proposals{};
    std::uint64_t cleanup_records{};
    std::uint64_t post_terminal_updates{4};
};
[[nodiscard]] Result<std::uint64_t> mandatory_event_slots(const EventReservation& bounds) noexcept;
[[nodiscard]] Result<std::uint64_t> mandatory_event_storage(const EventReservation& bounds,
    std::uint64_t ceiling = 64ULL * 1024 * 1024) noexcept;

} // namespace flamoris::runtime
