#pragma once

#include "flamoris/runtime/ids.hpp"
#include <optional>

namespace flamoris::runtime {

enum class EventKind {
    run_admitted,
    run_activity,
    run_terminal,
    job_created,
    job_state,
    job_terminal,
    attempt_started,
    attempt_finished,
    interrupt_requested,
    interrupt_applied,
    continuation_created,
    continuation_consumed,
    continuation_discarded,
    resource_reserved,
    resource_released,
    resource_quarantined,
    race_winner,
    result_rejected,
    reconciliation_updated,
    reconciliation_closed,
    telemetry_gap
};
// Minimal immutable domain value. Correlation/ordering is assigned by the owning
// controller at commit; no retained raw diagnostic strings are accepted here.
class EventValue final {
  public:
    [[nodiscard]] static Result<EventValue> make(EventKind kind, std::optional<JobId> job = {},
                                                 std::optional<AttemptId> attempt = {},
                                                 std::optional<ErrorCode> code = {}) noexcept {
        if (static_cast<unsigned>(kind) > static_cast<unsigned>(EventKind::telemetry_gap) ||
            (job && !job->valid()) || (attempt && (!job || !attempt->valid())) ||
            (code &&
             static_cast<unsigned>(*code) > static_cast<unsigned>(ErrorCode::run_cancelled)))
            return Result<EventValue>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
        return Result<EventValue>::success(EventValue(kind, job, attempt, code));
    }
    [[nodiscard]] EventKind kind() const noexcept { return kind_; }
    [[nodiscard]] std::optional<JobId> job() const noexcept { return job_; }
    [[nodiscard]] std::optional<AttemptId> attempt() const noexcept { return attempt_; }
    [[nodiscard]] std::optional<ErrorCode> code() const noexcept { return code_; }

  private:
    EventValue(EventKind kind, std::optional<JobId> job, std::optional<AttemptId> attempt,
               std::optional<ErrorCode> code) noexcept
        : kind_(kind), job_(job), attempt_(attempt), code_(code) {}
    EventKind kind_;
    std::optional<JobId> job_;
    std::optional<AttemptId> attempt_;
    std::optional<ErrorCode> code_;
};
static_assert(sizeof(EventValue) < 512);
static_assert(sizeof(ErrorEnvelope) < 2048);
} // namespace flamoris::runtime
