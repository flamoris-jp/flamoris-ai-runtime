#pragma once
#include "flamoris/runtime/compiler.hpp"
#include <functional>
namespace flamoris::runtime {
enum class AuthorizationBoundary { admission, dispatch, retry, resume };
enum class AccessSurface {
    status,
    result,
    events,
    trace,
    export_data,
    replay,
    cancel,
    resume,
    inject,
    handle
};
struct Confirmation {
    std::string subject, capability, input_digest;
    std::uint8_t effects{0};
    std::uint64_t expires_at_ms{0};
};
struct AuthorizationContext {
    std::string subject;
    std::uint64_t expires_at_ms{0};
    bool revoked{false};
    std::set<std::string> capabilities, object_scopes;
    std::set<AccessSurface> access;
    std::uint8_t permitted_effects{0};
    std::vector<Confirmation> confirmations;
};
struct PolicySnapshot {
    std::uint64_t revision{1};
    bool enabled{true};
    std::set<std::string> capabilities, object_scopes;
    std::set<AccessSurface> access;
    std::uint8_t permitted_effects{0}, confirmation_effects{0};
};
struct AuthorizationRequest {
    AuthorizationBoundary boundary{AuthorizationBoundary::dispatch};
    std::string capability, concrete_input_digest;
    JsonValue::Object concrete_inputs;
    // Optional additional restrictions; required scopes derive from registered metadata.
    std::vector<std::string> object_scopes;
    std::uint64_t now_ms{0}, deadline_ms{0};
    bool cancelled{false}, state_accessible{true}, epochs_current{true}, resources_ready{true};
};
struct AuthorizationDecision {
    std::uint64_t policy_revision;
    std::string concrete_input_digest;
};
// Called only by the owning serialized control executor. No historical allow is a grant.
class AuthorizationGate {
  public:
    Result<void> authorize_plan_admission(const AuthorizationContext &, const PolicySnapshot &,
                                          const ExecutionPlan &, const CapabilitySnapshot &,
                                          std::uint64_t now_ms) const;
    Result<AuthorizationDecision> check(const AuthorizationContext &, const PolicySnapshot &,
                                        const ExecutionPlan &, const CapabilitySnapshot &,
                                        const AuthorizationRequest &) const;
    Result<void> check_access(const AuthorizationContext &, const PolicySnapshot &,
                              std::string_view owner, AccessSurface, std::uint64_t now_ms,
                              std::optional<std::string_view> object_scope = {},
                              std::optional<std::uint64_t> handle_expiry = {}) const;
};
struct BudgetCharge {
    std::uint64_t jobs{0}, attempts{0}, proposals{0}, output_bytes{0}, control_steps{0},
        suspensions{0}, commands{0}, resource_operations{0};
};
class RunBudget {
  public:
    explicit RunBudget(RunLimits limits) : limits_(limits) {}
    Result<void> reserve(
        const BudgetCharge &); // all-or-none; cumulative charges are never refunded by cancellation
    const BudgetCharge &used() const noexcept { return used_; }
    // Caller also clamps timeout_ms to the original absolute deadline.
    RunLimits remaining() const noexcept;

  private:
    RunLimits limits_;
    BudgetCharge used_;
};
enum class AttemptOutcome {
    not_dispatched,
    confirmed_no_effect,
    confirmed_success,
    partial_effect,
    unknown
};
struct RetryEvidence {
    AttemptOutcome outcome{AttemptOutcome::unknown};
    bool previous_stopped{false}, reconciled{false}, transient{false};
    bool job_stopping_or_terminal{false};
    std::uint64_t completed_attempts{0}, max_attempts{1};
    std::string previous_provider_key, next_provider_key;
};
Result<void> authorize_retry(const CapabilityContract &, const RetryEvidence &,
                             std::uint64_t now_ms, std::uint64_t original_deadline_ms);
} // namespace flamoris::runtime
