#include "flamoris/runtime/registered_adapter.hpp"
#include <algorithm>
#include <stdexcept>

namespace flamoris::runtime {
namespace {
ErrorEnvelope failure(ErrorCode code, ExternalOutcome outcome = ExternalOutcome::not_dispatched,
                      ErrorStage stage = ErrorStage::dispatch) noexcept {
    return ErrorEnvelope::make(code, stage, outcome,
                               outcome == ExternalOutcome::unknown
                                   ? RetryDisposition::reconciliation_required
                                   : RetryDisposition::prohibited);
}
} // namespace
Result<AdapterGrant> AdapterAuthority::authorize(
    const AuthorizationContext &context, const PolicySnapshot &policy, const ExecutionPlan &plan,
    const CapabilitySnapshot &registry, const AuthorizationRequest &request, DispatchTicket ticket,
    const CapabilityPin &pin, const JsonValue &input, std::string operation_key,
    AdapterPurpose purpose, const RetryEvidence *retry) const {
    try {
        if (!ticket.job.valid() || !ticket.attempt || !ticket.dispatch_generation ||
            operation_key.empty() || operation_key.size() > 256 ||
            request.capability != pin.identifier)
            return Result<AdapterGrant>::failure(failure(ErrorCode::invalid_request));
        auto canonical = canonical_json(input);
        if (!canonical || canonical.value().size() > 1048576)
            return Result<AdapterGrant>::failure(failure(ErrorCode::invalid_request));
        auto digest = domain_digest("flamoris.operation-input/1\n", input);
        if (!digest || digest.value() != request.concrete_input_digest ||
            input != JsonValue(request.concrete_inputs))
            return Result<AdapterGrant>::failure(failure(ErrorCode::permission_denied));
        auto contract = registry.capabilities.find(pin.identifier);
        if (contract == registry.capabilities.end())
            return Result<AdapterGrant>::failure(failure(ErrorCode::unknown_capability));
        auto current_pin = fingerprint_capability(contract->second);
        if (!current_pin || current_pin.value() != pin ||
            std::find(plan.pins.begin(), plan.pins.end(), pin) == plan.pins.end())
            return Result<AdapterGrant>::failure(failure(ErrorCode::plan_stale));
        auto value = validate_value(input, contract->second.input_schema);
        if (!value)
            return Result<AdapterGrant>::failure(value.error());
        if (request.boundary == AuthorizationBoundary::admission)
            return Result<AdapterGrant>::failure(failure(ErrorCode::permission_denied));
        if (request.boundary == AuthorizationBoundary::retry) {
            if (!retry)
                return Result<AdapterGrant>::failure(failure(ErrorCode::permission_denied));
            auto permitted =
                authorize_retry(contract->second, *retry, request.now_ms, request.deadline_ms);
            if (!permitted)
                return Result<AdapterGrant>::failure(permitted.error());
        }
        auto decision = AuthorizationGate{}.check(context, policy, plan, registry, request);
        if (!decision)
            return Result<AdapterGrant>::failure(decision.error());
        auto expires = std::min(request.deadline_ms, context.expires_at_ms);
        if ((contract->second.effects.mask() & policy.confirmation_effects) != 0) {
            std::uint64_t confirmation_expiry = 0;
            for (const auto &confirmation : context.confirmations)
                if (confirmation.subject == context.subject &&
                    confirmation.capability == pin.identifier &&
                    confirmation.input_digest == digest.value() &&
                    (confirmation.effects & contract->second.effects.mask()) ==
                        contract->second.effects.mask())
                    confirmation_expiry = std::max(confirmation_expiry, confirmation.expires_at_ms);
            expires = std::min(expires, confirmation_expiry);
        }
        return Result<AdapterGrant>::success(
            AdapterGrant(ticket, pin, std::move(digest).value(), std::move(operation_key), expires,
                         decision.value().policy_revision, purpose, context.subject,
                         request.boundary == AuthorizationBoundary::retry));
    } catch (...) {
        return Result<AdapterGrant>::failure(failure(ErrorCode::internal_error));
    }
}

BoundedProviderSink::BoundedProviderSink(std::size_t limit)
    : limit_(std::min<std::size_t>(limit, 1048576)) {
    bytes_.reserve(limit_);
}
Result<void> BoundedProviderSink::append(std::string_view chunk) {
    if (error_)
        return Result<void>::failure(*error_);
    if (chunk.size() > limit_ - bytes_.size()) {
        error_ = failure(ErrorCode::result_too_large, outcome_, ErrorStage::result_validation);
        return Result<void>::failure(*error_);
    }
    try {
        bytes_.append(chunk);
        return Result<void>::success();
    } catch (...) {
        error_ = failure(ErrorCode::internal_error, outcome_, ErrorStage::result_validation);
        return Result<void>::failure(*error_);
    }
}
void BoundedProviderSink::record_outcome(ExternalOutcome outcome) noexcept {
    // Weakened or contradictory observations cannot erase confirmed effect evidence.
    if (outcome_ == ExternalOutcome::confirmed_success ||
        outcome_ == ExternalOutcome::confirmed_failure)
        return;
    if (outcome != ExternalOutcome::not_applicable)
        outcome_ = outcome;
}

RegisteredCapabilityAdapter::RegisteredCapabilityAdapter(CapabilityContract contract,
                                                         RegisteredProviderPort &provider,
                                                         std::size_t max_operations,
                                                         std::size_t max_queries)
    : contract_(std::move(contract)), provider_(provider), max_operations_(max_operations),
      max_reconciliation_queries_(max_queries) {
    if (!max_operations_ || max_operations_ > 65536 || max_queries > 1024 ||
        !contract_.max_output_bytes || contract_.max_output_bytes > 1048576)
        throw std::invalid_argument("invalid registered adapter bounds");
    auto pin = fingerprint_capability(contract_);
    if (!pin)
        throw std::invalid_argument("invalid registered adapter contract");
    pin_ = std::move(pin).value();
    consumed_.reserve(max_operations_);
    reconciled_.reserve(max_queries);
}

AdapterOutcome RegisteredCapabilityAdapter::invoke(AdapterRequest request,
                                                   std::uint64_t now) noexcept {
    AdapterOutcome outcome;
    outcome.ticket = request.grant.ticket_;
    try {
        auto reject = [&](ErrorCode code) {
            outcome.error = failure(code);
            return outcome;
        };
        if ((instance_ && *instance_ != request.grant.ticket_.job.instance()) ||
            request.grant.ticket_.job.run().value() < retired_before_)
            return reject(ErrorCode::permission_denied);
        if (request.grant.purpose_ != AdapterPurpose::invoke ||
            now >= request.grant.expires_at_ms_ || request.grant.policy_revision_ == 0)
            return reject(ErrorCode::permission_denied);
        if (request.grant.pin_ != pin_)
            return reject(ErrorCode::plan_stale);
        if (std::any_of(consumed_.begin(), consumed_.end(),
                        [&](const auto &record) { return record.ticket == request.grant.ticket_; }))
            return reject(ErrorCode::permission_denied);
        const auto prior =
            std::find_if(consumed_.begin(), consumed_.end(), [&](const auto &record) {
                return record.key == request.grant.operation_key_;
            });
        if (prior != consumed_.end() &&
            (!request.grant.retry_authorized_ || prior->digest != request.grant.input_digest_ ||
             prior->subject != request.grant.subject_ || !contract_.retry_permitted))
            return reject(ErrorCode::permission_denied);
        if (consumed_.size() == max_operations_)
            return reject(ErrorCode::budget_exceeded);
        auto digest = domain_digest("flamoris.operation-input/1\n", request.input);
        if (!digest || digest.value() != request.grant.input_digest_)
            return reject(ErrorCode::permission_denied);
        auto valid = validate_value(request.input, contract_.input_schema);
        if (!valid)
            return reject(ErrorCode::invalid_request);
        if (!request.max_output_bytes || request.max_output_bytes > 1048576)
            return reject(ErrorCode::invalid_request);
        BoundedProviderSink sink(
            std::min<std::size_t>(request.max_output_bytes, contract_.max_output_bytes));
        Invocation record{request.grant.ticket_, request.grant.operation_key_,
                          request.grant.input_digest_, request.grant.subject_};
        // Buffer and transcript capacity are secured before the one-use handoff.
        consumed_.push_back(std::move(record));
        instance_ = request.grant.ticket_.job.instance();
        struct Publication {
            Invocation &record;
            AdapterOutcome &outcome;
            ~Publication() noexcept { record.outcome = outcome.external_outcome; }
        } publication{consumed_.back(), outcome};
        ++dispatches_;
        outcome.handoff_attempted = true;
        outcome.external_outcome = ExternalOutcome::unknown;
        auto attach_retry = [&] {
            const auto proof = sink.retry_evidence();
            if (!proof.transient || !proof.previous_stopped || !proof.confirmed_no_effect ||
                (outcome.external_outcome != ExternalOutcome::not_dispatched &&
                 outcome.external_outcome != ExternalOutcome::confirmed_failure))
                return;
            RetryEvidence evidence;
            evidence.outcome = outcome.external_outcome == ExternalOutcome::not_dispatched
                                   ? AttemptOutcome::not_dispatched
                                   : AttemptOutcome::confirmed_no_effect;
            evidence.previous_stopped = true;
            evidence.reconciled = true;
            evidence.transient = true;
            evidence.completed_attempts = request.grant.ticket_.attempt;
            evidence.max_attempts = contract_.max_attempts;
            evidence.previous_provider_key = request.grant.operation_key_;
            evidence.next_provider_key = request.grant.operation_key_;
            outcome.retry_evidence = std::move(evidence);
            outcome.error =
                ErrorEnvelope::make(ErrorCode::upstream_failure, ErrorStage::execution,
                                    outcome.external_outcome, RetryDisposition::policy_eligible);
        };
        try {
            sink.record_outcome(provider_.invoke(request.grant.operation_key_, request.input,
                                                 request.grant.expires_at_ms_, sink));
        } catch (...) {
            outcome.external_outcome = sink.outcome();
            outcome.error = failure(outcome.external_outcome == ExternalOutcome::unknown
                                        ? ErrorCode::outcome_unknown
                                        : ErrorCode::upstream_failure,
                                    outcome.external_outcome, ErrorStage::execution);
            attach_retry();
            return outcome;
        }
        outcome.external_outcome = sink.outcome();
        if (sink.error()) {
            outcome.error = failure(sink.error()->code(), outcome.external_outcome,
                                    ErrorStage::result_validation);
            return outcome;
        }
        if (outcome.external_outcome != ExternalOutcome::confirmed_success) {
            outcome.error = failure(outcome.external_outcome == ExternalOutcome::unknown
                                        ? ErrorCode::outcome_unknown
                                        : ErrorCode::upstream_failure,
                                    outcome.external_outcome, ErrorStage::execution);
            attach_retry();
            return outcome;
        }
        JsonBounds bounds;
        bounds.max_bytes = request.max_output_bytes;
        bounds.max_string_bytes = std::min<std::size_t>(request.max_output_bytes, 262144);
        bounds.max_scalar_bytes = request.max_output_bytes;
        auto parsed = parse_bounded_json_value(sink.bytes(), bounds);
        if (!parsed || !validate_value(parsed.value(), contract_.output_schema)) {
            outcome.error = failure(ErrorCode::invalid_result, outcome.external_outcome,
                                    ErrorStage::result_validation);
            return outcome;
        }
        outcome.value = std::move(parsed).value();
        return outcome;
    } catch (...) {
        if (outcome.handoff_attempted &&
            outcome.external_outcome == ExternalOutcome::not_dispatched)
            outcome.external_outcome = ExternalOutcome::unknown;
        outcome.error =
            failure(ErrorCode::internal_error, outcome.external_outcome, ErrorStage::execution);
        return outcome;
    }
}

AdapterOutcome RegisteredCapabilityAdapter::reconcile(AdapterGrant grant,
                                                      std::uint64_t now) noexcept {
    AdapterOutcome outcome;
    outcome.ticket = grant.ticket_;
    try {
        if (grant.purpose_ != AdapterPurpose::reconcile || now >= grant.expires_at_ms_ ||
            grant.pin_ != pin_ || grant.policy_revision_ == 0) {
            outcome.error = failure(ErrorCode::permission_denied);
            return outcome;
        }
        auto invocation = std::find_if(consumed_.begin(), consumed_.end(), [&](const auto &record) {
            return record.ticket == grant.ticket_ && record.key == grant.operation_key_ &&
                   record.digest == grant.input_digest_ && record.subject == grant.subject_;
        });
        if (invocation == consumed_.end() || invocation->outcome != ExternalOutcome::unknown ||
            std::find(reconciled_.begin(), reconciled_.end(), grant.ticket_) != reconciled_.end() ||
            reconciled_.size() == max_reconciliation_queries_) {
            outcome.error = failure(ErrorCode::budget_exceeded);
            return outcome;
        }
        reconciled_.push_back(grant.ticket_);
        ++reconciliation_queries_;
        outcome.handoff_attempted = true;
        outcome.external_outcome = provider_.reconcile(grant.operation_key_, grant.expires_at_ms_);
        if (outcome.external_outcome == ExternalOutcome::not_applicable)
            outcome.external_outcome = ExternalOutcome::unknown;
        invocation->outcome = outcome.external_outcome;
        if (outcome.external_outcome == ExternalOutcome::unknown)
            outcome.error = failure(ErrorCode::outcome_unknown, ExternalOutcome::unknown,
                                    ErrorStage::execution);
        return outcome;
    } catch (...) {
        outcome.external_outcome = ExternalOutcome::unknown;
        outcome.error =
            failure(ErrorCode::outcome_unknown, ExternalOutcome::unknown, ErrorStage::execution);
        return outcome;
    }
}

bool RegisteredCapabilityAdapter::retire_before(RuntimeInstanceId instance,
                                                std::uint64_t minimum_live_run) noexcept {
    if (!instance.valid() || minimum_live_run < retired_before_ ||
        (instance_ && *instance_ != instance))
        return false;
    instance_ = instance;
    retired_before_ = minimum_live_run;
    consumed_.erase(std::remove_if(consumed_.begin(), consumed_.end(),
                                   [&](const auto &record) {
                                       return record.ticket.job.run().value() < retired_before_ &&
                                              record.outcome != ExternalOutcome::unknown;
                                   }),
                    consumed_.end());
    reconciled_.erase(std::remove_if(reconciled_.begin(), reconciled_.end(),
                                     [&](const auto &ticket) {
                                         return std::none_of(consumed_.begin(), consumed_.end(),
                                                             [&](const auto &record) {
                                                                 return record.ticket == ticket;
                                                             });
                                     }),
                      reconciled_.end());
    return true;
}

ExternalOutcome IdentityProvider::invoke(std::string_view, const JsonValue &input, std::uint64_t,
                                         BoundedProviderSink &sink) {
    auto value = canonical_json(input);
    if (!value)
        return ExternalOutcome::confirmed_failure;
    sink.record_outcome(ExternalOutcome::confirmed_success);
    (void)sink.append(value.value());
    return ExternalOutcome::confirmed_success;
}
ExternalOutcome IdentityProvider::reconcile(std::string_view, std::uint64_t) {
    return ExternalOutcome::unknown;
}
} // namespace flamoris::runtime
