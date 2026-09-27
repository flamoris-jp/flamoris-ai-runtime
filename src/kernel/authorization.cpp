#include "flamoris/runtime/authorization.hpp"
#include <cmath>
#include <limits>
namespace flamoris::runtime {
namespace {
ErrorEnvelope deny(ErrorCode c = ErrorCode::permission_denied) {
    return ErrorEnvelope::make(c, ErrorStage::dispatch);
}
bool handles_valid(const JsonValue &value, const ValueSchema &schema, const CapabilityContract &cap,
                   const AuthorizationContext &context, const PolicySnapshot &policy,
                   std::uint64_t now, unsigned depth = 0) {
    if (depth > 32)
        return false;
    if (schema.service_handle_type) {
        if (!cap.handle_validator || cap.handle_validator_revision.empty())
            return false;
        auto access = cap.handle_validator(value, context.subject, now);
        if (!access || access.value().owner != context.subject ||
            access.value().object_scope.empty() ||
            !context.object_scopes.contains(access.value().object_scope) ||
            !policy.object_scopes.contains(access.value().object_scope) ||
            now >= access.value().expires_at_ms)
            return false;
    }
    if (const auto *object = std::get_if<JsonValue::Object>(&value.data)) {
        for (const auto &[key, child] : *object) {
            auto it = schema.properties.find(key);
            if (it != schema.properties.end() &&
                !handles_valid(child, it->second, cap, context, policy, now, depth + 1))
                return false;
        }
    }
    if (const auto *array = std::get_if<JsonValue::Array>(&value.data)) {
        for (std::size_t i = 0; i < array->size(); ++i) {
            auto item = schema.tuple_items.empty()
                            ? schema.items.get()
                            : (i < schema.tuple_items.size() ? &schema.tuple_items[i] : nullptr);
            if (item && !handles_valid((*array)[i], *item, cap, context, policy, now, depth + 1))
                return false;
        }
    }
    return true;
}
} // namespace
Result<void> AuthorizationGate::authorize_plan_admission(const AuthorizationContext &context,
                                                         const PolicySnapshot &policy,
                                                         const ExecutionPlan &plan,
                                                         const CapabilitySnapshot &registry,
                                                         std::uint64_t now_ms) const {
    try {
        auto pins = verify_plan_pins(plan, registry);
        if (!pins)
            return pins;
        if (context.subject.empty() || context.subject.size() > 128 || context.revoked ||
            now_ms >= context.expires_at_ms || !policy.enabled || policy.revision == 0 ||
            plan.pins.empty())
            return Result<void>::failure(
                ErrorEnvelope::make(ErrorCode::permission_denied, ErrorStage::admission));
        for (const auto &pin : plan.pins) {
            const auto &cap = registry.capabilities.at(pin.identifier);
            const auto effects = cap.effects.mask();
            if (!context.capabilities.contains(pin.identifier) ||
                !policy.capabilities.contains(pin.identifier) ||
                (effects & context.permitted_effects) != effects ||
                (effects & policy.permitted_effects) != effects)
                return Result<void>::failure(
                    ErrorEnvelope::make(ErrorCode::permission_denied, ErrorStage::admission));
        }
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(
            ErrorEnvelope::make(ErrorCode::internal_error, ErrorStage::admission));
    }
}
Result<AuthorizationDecision> AuthorizationGate::check(const AuthorizationContext &context,
                                                       const PolicySnapshot &policy,
                                                       const ExecutionPlan &plan,
                                                       const CapabilitySnapshot &registry,
                                                       const AuthorizationRequest &request) const {
    try {
        auto pins = verify_plan_pins(plan, registry);
        if (!pins)
            return Result<AuthorizationDecision>::failure(pins.error());
        if (request.cancelled || request.now_ms >= request.deadline_ms)
            return Result<AuthorizationDecision>::failure(
                deny(request.cancelled ? ErrorCode::run_cancelled : ErrorCode::run_timeout));
        if (!request.state_accessible || !request.epochs_current)
            return Result<AuthorizationDecision>::failure(deny(ErrorCode::state_unavailable));
        if (!request.resources_ready)
            return Result<AuthorizationDecision>::failure(deny(ErrorCode::resource_unavailable));
        if (context.subject.empty() || context.revoked || request.now_ms >= context.expires_at_ms ||
            !policy.enabled)
            return Result<AuthorizationDecision>::failure(deny());
        bool pinned = false;
        for (const auto &pin : plan.pins)
            if (pin.identifier == request.capability)
                pinned = true;
        if (!pinned)
            return Result<AuthorizationDecision>::failure(deny());
        const auto found = registry.capabilities.find(request.capability);
        if (found == registry.capabilities.end())
            return Result<AuthorizationDecision>::failure(deny(ErrorCode::unknown_capability));
        if (!context.capabilities.contains(request.capability) ||
            !policy.capabilities.contains(request.capability))
            return Result<AuthorizationDecision>::failure(deny());
        const auto &contract = found->second;
        if (!validate_value(request.concrete_inputs, contract.input_schema))
            return Result<AuthorizationDecision>::failure(deny(ErrorCode::invalid_request));
        auto input_digest = domain_digest("flamoris.operation-input/1\n", request.concrete_inputs);
        if (!input_digest || (!request.concrete_input_digest.empty() &&
                              request.concrete_input_digest != input_digest.value()))
            return Result<AuthorizationDecision>::failure(deny());
        for (const auto &field : contract.object_scope_fields) {
            auto input = request.concrete_inputs.find(field);
            if (input == request.concrete_inputs.end())
                return Result<AuthorizationDecision>::failure(deny());
            const auto *scope = std::get_if<std::string>(&input->second.data);
            if (!scope || !context.object_scopes.contains(*scope) ||
                !policy.object_scopes.contains(*scope))
                return Result<AuthorizationDecision>::failure(deny());
        }
        if (!handles_valid(request.concrete_inputs, contract.input_schema, contract, context,
                           policy, request.now_ms))
            return Result<AuthorizationDecision>::failure(deny());
        const auto effects = found->second.effects.mask();
        if ((effects & context.permitted_effects) != effects ||
            (effects & policy.permitted_effects) != effects)
            return Result<AuthorizationDecision>::failure(deny());
        for (const auto &scope : request.object_scopes)
            if (!context.object_scopes.contains(scope) || !policy.object_scopes.contains(scope))
                return Result<AuthorizationDecision>::failure(deny());
        if ((effects & policy.confirmation_effects) != 0) {
            bool confirmed = false;
            for (const auto &c : context.confirmations)
                if (c.subject == context.subject && c.capability == request.capability &&
                    c.input_digest == input_digest.value() && (effects & c.effects) == effects &&
                    request.now_ms < c.expires_at_ms)
                    confirmed = true;
            if (!confirmed)
                return Result<AuthorizationDecision>::failure(deny());
        }
        return Result<AuthorizationDecision>::success(
            {policy.revision, std::move(input_digest.value())});
    } catch (...) {
        return Result<AuthorizationDecision>::failure(deny(ErrorCode::internal_error));
    }
}
Result<void> AuthorizationGate::check_access(const AuthorizationContext &context,
                                             const PolicySnapshot &policy, std::string_view owner,
                                             AccessSurface surface, std::uint64_t now,
                                             std::optional<std::string_view> scope,
                                             std::optional<std::uint64_t> expiry) const {
    try {
        if (context.subject.empty() || context.subject != owner || context.revoked ||
            now >= context.expires_at_ms || !policy.enabled || !context.access.contains(surface) ||
            !policy.access.contains(surface) || (expiry && now >= *expiry))
            return Result<void>::failure(deny());
        if (scope && (!context.object_scopes.contains(std::string(*scope)) ||
                      !policy.object_scopes.contains(std::string(*scope))))
            return Result<void>::failure(deny());
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(deny(ErrorCode::internal_error));
    }
}
Result<void> RunBudget::reserve(const BudgetCharge &c) {
    const std::uint64_t values[] = {used_.jobs,         used_.attempts,           used_.proposals,
                                    used_.output_bytes, used_.control_steps,      used_.suspensions,
                                    used_.commands,     used_.resource_operations};
    const std::uint64_t charges[] = {c.jobs,         c.attempts,           c.proposals,
                                     c.output_bytes, c.control_steps,      c.suspensions,
                                     c.commands,     c.resource_operations};
    const std::uint64_t limits[] = {limits_.max_jobs,
                                    limits_.max_attempts,
                                    limits_.max_dynamic_proposals,
                                    limits_.max_output_bytes,
                                    limits_.max_control_steps,
                                    limits_.max_suspensions,
                                    limits_.max_control_commands,
                                    limits_.max_resource_operations};
    for (unsigned i = 0; i < 8; ++i)
        if (values[i] > limits[i] || charges[i] > limits[i] - values[i])
            return Result<void>::failure(deny(ErrorCode::budget_exceeded));
    used_.jobs += c.jobs;
    used_.attempts += c.attempts;
    used_.proposals += c.proposals;
    used_.output_bytes += c.output_bytes;
    used_.control_steps += c.control_steps;
    used_.suspensions += c.suspensions;
    used_.commands += c.commands;
    used_.resource_operations += c.resource_operations;
    return Result<void>::success();
}
RunLimits RunBudget::remaining() const noexcept {
    auto remaining = limits_;
    remaining.max_jobs -= used_.jobs;
    remaining.max_attempts -= used_.attempts;
    remaining.max_dynamic_proposals -= used_.proposals;
    remaining.max_output_bytes -= used_.output_bytes;
    remaining.max_control_steps -= used_.control_steps;
    remaining.max_suspensions -= used_.suspensions;
    remaining.max_control_commands -= used_.commands;
    remaining.max_resource_operations -= used_.resource_operations;
    return remaining;
}
Result<void> authorize_retry(const CapabilityContract &cap, const RetryEvidence &e,
                             std::uint64_t now, std::uint64_t deadline) {
    if (now >= deadline)
        return Result<void>::failure(deny(ErrorCode::job_timeout));
    if (!cap.retry_permitted || !e.previous_stopped || e.job_stopping_or_terminal ||
        e.completed_attempts >= e.max_attempts || e.completed_attempts >= cap.max_attempts ||
        !e.transient || e.previous_provider_key != e.next_provider_key)
        return Result<void>::failure(deny());
    if (e.outcome == AttemptOutcome::confirmed_success ||
        e.outcome == AttemptOutcome::partial_effect)
        return Result<void>::failure(deny());
    if (e.outcome == AttemptOutcome::unknown &&
        (!cap.provider_deduplication || !e.reconciled || e.previous_provider_key.empty()))
        return Result<void>::failure(deny(ErrorCode::outcome_unknown));
    return Result<void>::success();
}
} // namespace flamoris::runtime
