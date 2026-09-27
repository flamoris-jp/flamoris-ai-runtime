#pragma once

#include "flamoris/runtime/clock.hpp"
#include "flamoris/runtime/ids.hpp"
#include "flamoris/runtime/limits.hpp"
#include <algorithm>
#include <functional>
#include <vector>

namespace flamoris::runtime::testing {

class DeterministicIdSource final : public IdSource {
  public:
    explicit DeterministicIdSource(std::uint64_t seed = 1) noexcept : seed_(seed) {}
    Result<RuntimeInstanceId> next_instance() noexcept override {
        auto next = counter_.next();
        if (!next)
            return Result<RuntimeInstanceId>::failure(next.error());
        return Result<RuntimeInstanceId>::success(RuntimeInstanceId{seed_, next.value()});
    }

  private:
    std::uint64_t seed_;
    CheckedCounter counter_;
};

class ManualClock final : public MonotonicClock {
  public:
    [[nodiscard]] TimePoint now() const noexcept override { return now_; }
    [[nodiscard]] Result<void> advance(Duration duration) noexcept {
        if (duration.count() < 0 ||
            duration.count() > std::numeric_limits<Duration::rep>::max() - now_.count())
            return Result<void>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
        now_ += duration;
        return Result<void>::success();
    }

  private:
    TimePoint now_{};
};

// Advancing time and delivering an elapsed timer are deliberately independent.
// Tests can accept another command at exact expiry before delivering its timer.
class ManualTimerQueue final {
  public:
    explicit ManualTimerQueue(std::size_t capacity) : capacity_(capacity) {
        timers_.reserve(capacity);
    }
    [[nodiscard]] Result<OperationId> schedule(Deadline deadline, std::function<void()> callback) {
        if (timers_.size() == capacity_ || !callback)
            return Result<OperationId>::failure(ErrorEnvelope::make(ErrorCode::budget_exceeded));
        auto id = ids_.next();
        if (!id)
            return Result<OperationId>::failure(id.error());
        timers_.push_back(Timer{OperationId{id.value()}, deadline, std::move(callback)});
        return Result<OperationId>::success(OperationId{id.value()});
    }
    [[nodiscard]] bool deliver(OperationId id, TimePoint now) {
        auto it = std::find_if(timers_.begin(), timers_.end(),
                               [id](const Timer &t) { return t.id == id; });
        if (it == timers_.end() || !it->deadline.expired(now))
            return false;
        auto callback = std::move(it->callback);
        timers_.erase(it);
        callback();
        return true;
    }
    [[nodiscard]] std::size_t size() const noexcept { return timers_.size(); }

  private:
    struct Timer {
        OperationId id;
        Deadline deadline;
        std::function<void()> callback;
    };
    std::size_t capacity_;
    CheckedCounter ids_;
    std::vector<Timer> timers_;
};

} // namespace flamoris::runtime::testing
