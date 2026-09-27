#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

namespace flamoris::runtime {

enum class ErrorCode {
    invalid_request,
    invalid_workflow,
    invalid_reference,
    unknown_capability,
    plan_stale,
    permission_denied,
    budget_exceeded,
    unsupported_model,
    capability_unavailable,
    native_compute_unavailable,
    resource_unavailable,
    native_execution_failure,
    upstream_failure,
    state_unavailable,
    invalid_result,
    result_too_large,
    job_timeout,
    run_timeout,
    outcome_unknown,
    cleanup_failed,
    cleanup_timeout,
    internal_error,
    invariant_violation,
    race_no_acceptable_result,
    job_cancelled,
    run_cancelled
};
enum class ErrorCategory {
    input_plan,
    policy,
    availability,
    execution,
    contract,
    deadline,
    uncertainty,
    containment,
    internal,
    cancellation
};
enum class ErrorStage { validation, admission, dispatch, execution, result_validation, cleanup };
enum class ExternalOutcome {
    not_applicable,
    not_dispatched,
    confirmed_success,
    confirmed_failure,
    unknown
};
enum class RetryDisposition { prohibited, policy_eligible, reconciliation_required };
enum class ErrorReason {
    none,
    invalid_transition,
    stale_observation,
    unsupported_operation,
    counter_exhausted,
    limit_exceeded,
    version_mismatch,
    resource_deadlock,
    malformed_identifier,
    invalid_effects,
    arithmetic_overflow
};

[[nodiscard]] std::string_view to_string(ErrorCode code) noexcept;
[[nodiscard]] std::string_view to_string(ErrorCategory category) noexcept;
[[nodiscard]] std::string_view to_string(ErrorStage stage) noexcept;
[[nodiscard]] std::string_view to_string(ExternalOutcome outcome) noexcept;
[[nodiscard]] std::string_view to_string(RetryDisposition retry) noexcept;
[[nodiscard]] std::string_view to_string(ErrorReason reason) noexcept;

// Only Runtime-owned classifications enter retained error values. No raw upstream text,
// endpoint, path or nested exception is accepted by this factory.
class ErrorEnvelope final {
  public:
    static constexpr std::string_view schema_version = "flamoris.error/1";
    static constexpr std::size_t max_causes = 8;
    [[nodiscard]] static ErrorEnvelope
    make(ErrorCode code, ErrorStage stage = ErrorStage::validation,
         ExternalOutcome outcome = ExternalOutcome::not_applicable,
         RetryDisposition retry = RetryDisposition::prohibited,
         ErrorReason reason = ErrorReason::none, std::span<const ErrorCode> causes = {}) noexcept;

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }
    [[nodiscard]] ErrorCategory category() const noexcept;
    [[nodiscard]] std::string_view message() const noexcept;
    [[nodiscard]] ErrorStage stage() const noexcept { return stage_; }
    [[nodiscard]] ExternalOutcome external_outcome() const noexcept { return outcome_; }
    [[nodiscard]] RetryDisposition retry_disposition() const noexcept { return retry_; }
    [[nodiscard]] ErrorReason reason() const noexcept { return reason_; }
    [[nodiscard]] std::span<const ErrorCode> cause_codes() const noexcept {
        return {causes_.data(), cause_count_};
    }
    [[nodiscard]] bool operator==(const ErrorEnvelope &) const = default;

  private:
    ErrorEnvelope(ErrorCode code, ErrorStage stage, ExternalOutcome outcome, RetryDisposition retry,
                  ErrorReason reason) noexcept
        : code_(code), stage_(stage), outcome_(outcome), retry_(retry), reason_(reason) {}
    ErrorCode code_;
    ErrorStage stage_;
    ExternalOutcome outcome_;
    RetryDisposition retry_;
    ErrorReason reason_;
    std::array<ErrorCode, max_causes> causes_{};
    std::size_t cause_count_{};
};

} // namespace flamoris::runtime
