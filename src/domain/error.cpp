#include "flamoris/runtime/error.hpp"
#include <algorithm>
#include <array>

namespace flamoris::runtime {
namespace {
struct ErrorInfo {
    std::string_view name;
    ErrorCategory category;
    std::string_view message;
};
constexpr std::array entries{
    ErrorInfo{"invalid_request", ErrorCategory::input_plan, "The request is invalid."},
    ErrorInfo{"invalid_workflow", ErrorCategory::input_plan, "The workflow is invalid."},
    ErrorInfo{"invalid_reference", ErrorCategory::input_plan, "The reference is invalid."},
    ErrorInfo{"unknown_capability", ErrorCategory::input_plan, "The capability is not registered."},
    ErrorInfo{"plan_stale", ErrorCategory::input_plan,
              "The plan no longer matches current contracts."},
    ErrorInfo{"permission_denied", ErrorCategory::policy,
              "Current authorization denies the operation."},
    ErrorInfo{"budget_exceeded", ErrorCategory::policy,
              "The finite operation budget is exhausted."},
    ErrorInfo{"unsupported_model", ErrorCategory::availability,
              "The model profile is unsupported."},
    ErrorInfo{"capability_unavailable", ErrorCategory::availability,
              "The capability is unavailable."},
    ErrorInfo{"native_compute_unavailable", ErrorCategory::availability,
              "Native compute is unavailable."},
    ErrorInfo{"resource_unavailable", ErrorCategory::availability,
              "Required resource capacity is unavailable."},
    ErrorInfo{"native_execution_failure", ErrorCategory::execution, "Native execution failed."},
    ErrorInfo{"upstream_failure", ErrorCategory::execution,
              "The registered external operation failed."},
    ErrorInfo{"state_unavailable", ErrorCategory::execution,
              "Required execution state is unavailable."},
    ErrorInfo{"invalid_result", ErrorCategory::contract,
              "The result does not satisfy its contract."},
    ErrorInfo{"result_too_large", ErrorCategory::contract, "The result exceeds its size limit."},
    ErrorInfo{"job_timeout", ErrorCategory::deadline, "The Job deadline has elapsed."},
    ErrorInfo{"run_timeout", ErrorCategory::deadline, "The Run deadline has elapsed."},
    ErrorInfo{"outcome_unknown", ErrorCategory::uncertainty, "The external outcome is unknown."},
    ErrorInfo{"cleanup_failed", ErrorCategory::containment,
              "Cleanup failed; ownership remains accounted."},
    ErrorInfo{"cleanup_timeout", ErrorCategory::containment,
              "Cleanup timed out; ownership remains accounted."},
    ErrorInfo{"internal_error", ErrorCategory::internal, "The operation failed internally."},
    ErrorInfo{"invariant_violation", ErrorCategory::internal,
              "An execution invariant was violated."},
    ErrorInfo{"race_no_acceptable_result", ErrorCategory::execution,
              "No race participant produced an acceptable result."},
    ErrorInfo{"job_cancelled", ErrorCategory::cancellation, "The Job cancellation is committed."},
    ErrorInfo{"run_cancelled", ErrorCategory::cancellation, "The Run cancellation is committed."}};
const ErrorInfo &info(ErrorCode code) noexcept {
    const auto index = static_cast<std::size_t>(code);
    return entries[index < entries.size() ? index
                                          : static_cast<std::size_t>(ErrorCode::internal_error)];
}
template <class E, std::size_t N>
std::string_view enum_name(E value, const std::array<std::string_view, N> &names) noexcept {
    const auto index = static_cast<std::size_t>(value);
    return index < N ? names[index] : std::string_view{"unknown"};
}
} // namespace

std::string_view to_string(ErrorCode code) noexcept { return info(code).name; }
std::string_view to_string(ErrorCategory value) noexcept {
    return enum_name(value, std::array<std::string_view, 10>{"input_plan", "policy", "availability",
                                                             "execution", "contract", "deadline",
                                                             "uncertainty", "containment",
                                                             "internal", "cancellation"});
}
std::string_view to_string(ErrorStage value) noexcept {
    return enum_name(value,
                     std::array<std::string_view, 6>{"validation", "admission", "dispatch",
                                                     "execution", "result_validation", "cleanup"});
}
std::string_view to_string(ExternalOutcome value) noexcept {
    return enum_name(value, std::array<std::string_view, 5>{"not_applicable", "not_dispatched",
                                                            "confirmed_success",
                                                            "confirmed_failure", "unknown"});
}
std::string_view to_string(RetryDisposition value) noexcept {
    return enum_name(value, std::array<std::string_view, 3>{"prohibited", "policy_eligible",
                                                            "reconciliation_required"});
}
std::string_view to_string(ErrorReason value) noexcept {
    return enum_name(value, std::array<std::string_view, 11>{
                                "none", "invalid_transition", "stale_observation",
                                "unsupported_operation", "counter_exhausted", "limit_exceeded",
                                "version_mismatch", "resource_deadlock", "malformed_identifier",
                                "invalid_effects", "arithmetic_overflow"});
}
ErrorEnvelope ErrorEnvelope::make(ErrorCode code, ErrorStage stage, ExternalOutcome outcome,
                                  RetryDisposition retry, ErrorReason reason,
                                  std::span<const ErrorCode> causes) noexcept {
    if (static_cast<std::size_t>(code) >= entries.size())
        code = ErrorCode::internal_error;
    if (static_cast<unsigned>(stage) > static_cast<unsigned>(ErrorStage::cleanup))
        stage = ErrorStage::validation;
    if (static_cast<unsigned>(outcome) > static_cast<unsigned>(ExternalOutcome::unknown))
        outcome = ExternalOutcome::unknown;
    if (static_cast<unsigned>(retry) >
        static_cast<unsigned>(RetryDisposition::reconciliation_required))
        retry = RetryDisposition::prohibited;
    if (static_cast<unsigned>(reason) > static_cast<unsigned>(ErrorReason::arithmetic_overflow))
        reason = ErrorReason::none;
    if (outcome == ExternalOutcome::unknown)
        retry = RetryDisposition::reconciliation_required;
    ErrorEnvelope result(code, stage, outcome, retry, reason);
    result.cause_count_ = std::min(causes.size(), max_causes);
    for (std::size_t i = 0; i < result.cause_count_; ++i)
        result.causes_[i] = static_cast<std::size_t>(causes[i]) < entries.size()
                                ? causes[i]
                                : ErrorCode::internal_error;
    return result;
}
ErrorCategory ErrorEnvelope::category() const noexcept { return info(code_).category; }
std::string_view ErrorEnvelope::message() const noexcept { return info(code_).message; }

} // namespace flamoris::runtime
