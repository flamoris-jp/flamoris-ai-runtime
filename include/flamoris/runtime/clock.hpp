#pragma once

#include "flamoris/runtime/result.hpp"
#include <chrono>
#include <cstdint>
#include <limits>

namespace flamoris::runtime {

using TimePoint = std::chrono::nanoseconds;
using Duration = std::chrono::nanoseconds;
class MonotonicClock {
public:
    virtual ~MonotonicClock() = default;
    [[nodiscard]] virtual TimePoint now() const noexcept = 0;
};
class SteadyMonotonicClock final : public MonotonicClock {
public:
    SteadyMonotonicClock() noexcept : origin_(std::chrono::steady_clock::now()) {}
    [[nodiscard]] TimePoint now() const noexcept override {
        return std::chrono::duration_cast<TimePoint>(std::chrono::steady_clock::now() - origin_);
    }
private:
    std::chrono::steady_clock::time_point origin_;
};
class Deadline final {
public:
    [[nodiscard]] static constexpr Deadline at(TimePoint time) noexcept { return Deadline(time); }
    [[nodiscard]] static Result<Deadline> after(TimePoint now, Duration budget) noexcept {
        if (now.count() < 0 || budget.count() <= 0 ||
            budget.count() > std::numeric_limits<Duration::rep>::max() - now.count())
            return Result<Deadline>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
        return Result<Deadline>::success(Deadline(now + budget));
    }
    [[nodiscard]] constexpr bool expired(TimePoint now) const noexcept { return now >= time_; }
    [[nodiscard]] constexpr TimePoint time() const noexcept { return time_; }
private:
    explicit constexpr Deadline(TimePoint time) noexcept : time_(time) {}
    TimePoint time_;
};
struct CleanupDeadline final {
    Deadline deadline;
};

} // namespace flamoris::runtime
