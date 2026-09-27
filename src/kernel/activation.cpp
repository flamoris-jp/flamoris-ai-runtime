#include "flamoris/runtime/activation.hpp"

#include <algorithm>
#include <limits>

namespace flamoris::runtime {
namespace {
template <class T> Result<T> unavailable() {
    return Result<T>::failure(
        ErrorEnvelope::make(ErrorCode::native_compute_unavailable, ErrorStage::admission));
}
template <class T> Result<T> fail(ErrorCode code) {
    return Result<T>::failure(ErrorEnvelope::make(code, ErrorStage::admission));
}
Result<void> authorize(ActivationPolicyPort &policy, ActivationScope scope,
                       const ActivationKey &key) {
    try {
        return policy.authorize(scope, key);
    } catch (...) {
        return Result<void>::failure(ErrorEnvelope::make(
            ErrorCode::permission_denied, ErrorStage::admission, ExternalOutcome::not_dispatched));
    }
}
template <class F> Result<void> outbound(F &&send) {
    try {
        return send();
    } catch (...) {
        return Result<void>::failure(ErrorEnvelope::make(
            ErrorCode::outcome_unknown, ErrorStage::admission, ExternalOutcome::unknown,
            RetryDisposition::reconciliation_required));
    }
}
bool valid_key(const ActivationKey &key) {
    return !key.service.empty() && key.service.size() <= 64 && key.configuration_revision != 0 &&
           key.ownership_scope != 0 &&
           std::all_of(key.service.begin(), key.service.end(), [](unsigned char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                      c == '.' || c == '_' || c == '-';
           });
}
bool next_status(ProcessStatus from, ProcessStatus to) {
    if (from == to)
        return true;
    if (to == ProcessStatus::unknown)
        return true;
    switch (from) {
    case ProcessStatus::requested:
        return to == ProcessStatus::starting || to == ProcessStatus::ready ||
               to == ProcessStatus::failed;
    case ProcessStatus::starting:
        return to == ProcessStatus::ready || to == ProcessStatus::stopping ||
               to == ProcessStatus::failed;
    case ProcessStatus::ready:
        return to == ProcessStatus::draining;
    case ProcessStatus::draining:
        return to == ProcessStatus::stopping;
    case ProcessStatus::stopping:
        return to == ProcessStatus::stopped || to == ProcessStatus::failed;
    case ProcessStatus::stopped:
    case ProcessStatus::failed:
    case ProcessStatus::unknown:
        return false;
    }
    return false;
}
} // namespace
Result<void> UnavailableActivationGateway::request(const ActivationRequest &) {
    return Result<void>::failure(ErrorEnvelope::make(ErrorCode::native_compute_unavailable,
                                                     ErrorStage::admission,
                                                     ExternalOutcome::not_dispatched));
}
Result<void> UnavailableActivationGateway::detach_waiter(OperationId, const ActivationKey &) {
    return unavailable<void>();
}
Result<void> UnavailableActivationGateway::request_stop(const ActivationObservation &, TimePoint) {
    return unavailable<void>();
}
ActivationClient::ActivationClient(HostActivationPort &gateway, ActivationPolicyPort &policy,
                                   std::size_t max_requests)
    : gateway_(gateway), policy_(policy), limit_(max_requests) {
    pending_.reserve(limit_);
}
Result<void> ActivationClient::request(const ActivationRequest &request, TimePoint now) {
    if (!request.request.valid() || !request.expected_epoch.valid() || !valid_key(request.key) ||
        request.deadline <= now)
        return fail<void>(ErrorCode::invalid_request);
    auto permission = authorize(policy_, ActivationScope::activate, request.key);
    if (!permission)
        return permission;
    auto it = std::find_if(pending_.begin(), pending_.end(),
                           [&](const auto &p) { return p.request.request == request.request; });
    if (it != pending_.end()) {
        return it->request.key == request.key &&
                       it->request.expected_epoch == request.expected_epoch &&
                       it->request.deadline == request.deadline
                   ? Result<void>::success()
                   : fail<void>(ErrorCode::invalid_request);
    }
    if (request.request.value() <= operation_high_water_)
        return fail<void>(ErrorCode::invalid_request);
    if (pending_.size() >= limit_)
        return fail<void>(ErrorCode::resource_unavailable);
    Pending pending{request,
                    {request.request,
                     request.key,
                     {},
                     request.expected_epoch,
                     ProcessStatus::requested,
                     {},
                     0,
                     false},
                    false,
                    false};
    pending_.push_back(std::move(pending));
    operation_high_water_ = request.request.value();
    auto sent = outbound([&] { return gateway_.request(request); });
    if (!sent) {
        if (sent.error().external_outcome() == ExternalOutcome::not_dispatched) {
            pending_.pop_back();
            // The identity is consumed even though no process was started; callers
            // make an explicitly new bounded attempt with a fresh operation ID.
        } else
            pending_.back().current.status = ProcessStatus::unknown;
        return sent;
    }
    return Result<void>::success();
}
Result<void> ActivationClient::observe(const ActivationObservation &observation, TimePoint now) {
    auto it = std::find_if(pending_.begin(), pending_.end(),
                           [&](const auto &p) { return p.request.request == observation.request; });
    if (it == pending_.end() || observation.key != it->request.key ||
        !observation.generation.valid() || observation.host_epoch != it->request.expected_epoch ||
        (it->current.generation.valid() && observation.generation != it->current.generation))
        return fail<void>(ErrorCode::state_unavailable);
    if (it->current == observation)
        return Result<void>::success();
    if (observation.status == ProcessStatus::ready &&
        (now >= it->request.deadline || it->stop_requested || !observation.instance.valid() ||
         observation.admission_epoch == 0))
        return unavailable<void>();
    if (observation.status == ProcessStatus::stopped && !observation.safe_stop_confirmed)
        return fail<void>(ErrorCode::cleanup_failed);
    if (it->current.instance.valid() && observation.instance != it->current.instance)
        return fail<void>(ErrorCode::state_unavailable);
    if (it->current.status == ProcessStatus::ready && observation.status == ProcessStatus::ready)
        return fail<void>(ErrorCode::state_unavailable);
    if (!next_status(it->current.status, observation.status))
        return fail<void>(ErrorCode::state_unavailable);
    // Readiness is host evidence; it still confers no model/submit/inspect permission.
    it->current = observation;
    return Result<void>::success();
}
Result<void> ActivationClient::detach(OperationId request) {
    auto it = std::find_if(pending_.begin(), pending_.end(),
                           [&](const auto &p) { return p.request.request == request; });
    if (it == pending_.end())
        return fail<void>(ErrorCode::invalid_request);
    if (it->detached)
        return Result<void>::success();
    auto detached = outbound([&] { return gateway_.detach_waiter(request, it->request.key); });
    if (!detached)
        return detached;
    it->detached = true;
    return Result<void>::success();
}
Result<void> ActivationClient::request_stop(OperationId request, TimePoint deadline,
                                            TimePoint now) {
    auto it = std::find_if(pending_.begin(), pending_.end(),
                           [&](const auto &p) { return p.request.request == request; });
    if (it == pending_.end() || deadline <= now)
        return fail<void>(ErrorCode::invalid_request);
    auto permission = authorize(policy_, ActivationScope::stop, it->request.key);
    if (!permission)
        return permission;
    if (it->stop_requested)
        return Result<void>::success();
    if (it->current.status != ProcessStatus::ready && it->current.status != ProcessStatus::starting)
        return unavailable<void>();
    it->stop_requested = true;
    auto sent = outbound([&] { return gateway_.request_stop(it->current, deadline); });
    if (!sent) {
        if (sent.error().external_outcome() == ExternalOutcome::not_dispatched)
            it->stop_requested = false;
        else
            it->current.status = ProcessStatus::unknown;
        return sent;
    }
    return Result<void>::success();
}
Result<ActivationObservation> ActivationClient::inspect(OperationId request) {
    auto it = std::find_if(pending_.begin(), pending_.end(),
                           [&](const auto &p) { return p.request.request == request; });
    if (it == pending_.end())
        return fail<ActivationObservation>(ErrorCode::invalid_request);
    auto permission = authorize(policy_, ActivationScope::inspect, it->request.key);
    if (!permission)
        return Result<ActivationObservation>::failure(permission.error());
    return Result<ActivationObservation>::success(it->current);
}
Result<void> ActivationClient::authorize_model(OperationId request) {
    auto it = std::find_if(pending_.begin(), pending_.end(),
                           [&](const auto &p) { return p.request.request == request; });
    if (it == pending_.end())
        return fail<void>(ErrorCode::invalid_request);
    auto permission = authorize(policy_, ActivationScope::model, it->request.key);
    if (!permission)
        return permission;
    return it->current.status == ProcessStatus::ready ? Result<void>::success()
                                                      : unavailable<void>();
}
void ActivationClient::host_epoch_lost(HostEpoch epoch) noexcept {
    for (auto &p : pending_)
        if (p.request.expected_epoch == epoch && p.current.status != ProcessStatus::stopped)
            p.current.status = ProcessStatus::unknown;
}
void ActivationClient::expire(TimePoint now) noexcept {
    for (auto &p : pending_)
        if (now >= p.request.deadline && (p.current.status == ProcessStatus::requested ||
                                          p.current.status == ProcessStatus::starting))
            p.current.status = ProcessStatus::unknown;
}
std::size_t ActivationClient::retire_detached() noexcept {
    const auto old_size = pending_.size();
    std::erase_if(pending_, [](const auto &p) {
        return p.detached || p.current.status == ProcessStatus::stopped;
    });
    return old_size - pending_.size();
}
bool IdleSnapshot::idle() const noexcept {
    return nonterminal_jobs == 0 && duplicate_waiters == 0 && prepared_dispatches == 0 &&
           model_loads == 0 && controller_maintenance == 0 && nontransferable_cleanup == 0;
}
AdmissionGate::AdmissionGate(std::uint64_t initial_epoch, std::size_t max_pending)
    : epoch_(initial_epoch), limit_(max_pending) {
    claims_.reserve(limit_);
    if (initial_epoch == 0)
        state_ = AdmissionGateState::closed;
}
Result<std::uint64_t> AdmissionGate::claim() {
    if (state_ != AdmissionGateState::accepting)
        return unavailable<std::uint64_t>();
    if (claims_.size() >= limit_ || next_claim_ == std::numeric_limits<std::uint64_t>::max())
        return fail<std::uint64_t>(ErrorCode::resource_unavailable);
    const auto id = next_claim_++;
    claims_.push_back(id);
    return Result<std::uint64_t>::success(id);
}
Result<void> AdmissionGate::settle_claim(std::uint64_t claim) {
    auto it = std::find(claims_.begin(), claims_.end(), claim);
    if (it == claims_.end())
        return fail<void>(ErrorCode::invalid_request);
    claims_.erase(it);
    return Result<void>::success();
}
Result<std::uint64_t> AdmissionGate::prepare_idle_stop(std::uint64_t expected_epoch,
                                                       const IdleSnapshot &snapshot) {
    if (state_ != AdmissionGateState::accepting || expected_epoch != epoch_ || !claims_.empty() ||
        !snapshot.idle())
        return unavailable<std::uint64_t>();
    if (epoch_ == std::numeric_limits<std::uint64_t>::max())
        return fail<std::uint64_t>(ErrorCode::resource_unavailable);
    state_ = AdmissionGateState::draining;
    ++epoch_;
    return Result<std::uint64_t>::success(epoch_);
}
Result<void> AdmissionGate::shutdown_confirmed(ContainmentProof proof) {
    if (state_ != AdmissionGateState::draining ||
        (proof != ContainmentProof::worker_quiesced &&
         proof != ContainmentProof::isolated_by_authority))
        return fail<void>(ErrorCode::cleanup_failed);
    state_ = AdmissionGateState::closed;
    return Result<void>::success();
}
bool may_retry_submission(ForwardedAdmission outcome, RuntimeInstanceId original,
                          RuntimeInstanceId current, bool retained_key) noexcept {
    if (outcome == ForwardedAdmission::not_sent ||
        outcome == ForwardedAdmission::rejected_before_run)
        return true;
    return outcome == ForwardedAdmission::unknown && original == current && original.valid() &&
           retained_key;
}
ModelLoadRegistry::ModelLoadRegistry(ResourceManager &resources, std::size_t max_models)
    : resources_(resources), limit_(max_models) {
    models_.reserve(limit_);
}
ModelLoadDecision ModelLoadRegistry::availability(const ModelResidencyKey &key) const noexcept {
    auto it = std::find_if(models_.begin(), models_.end(),
                           [&](const auto &model) { return model.snapshot.key == key; });
    if (it == models_.end() || it->snapshot.state == ModelResidencyState::absent)
        return ModelLoadDecision::load_owned;
    return it->snapshot.state == ModelResidencyState::resident
               ? ModelLoadDecision::resident_available
               : ModelLoadDecision::wait_without_lease;
}
Result<void> ModelLoadRegistry::begin(const ModelResidencyKey &key, ResourceOwner owner,
                                      const ExecutionLease &lease, bool pausable,
                                      TimePoint deadline, TimePoint now) {
    if (owner != lease.ticket.owner || key.resource != lease.ticket.resource ||
        key.host_epoch != lease.ticket.host_epoch || key.worker != lease.ticket.worker ||
        deadline <= now || !resources_.lease_current(lease, now))
        return fail<void>(ErrorCode::state_unavailable);
    auto it = std::find_if(models_.begin(), models_.end(),
                           [&](const auto &model) { return model.snapshot.key == key; });
    if (it != models_.end() && it->snapshot.state != ModelResidencyState::absent)
        return fail<void>(ErrorCode::resource_unavailable);
    ModelRecord model{
        {key, ModelResidencyState::loading, owner, std::nullopt, pausable}, lease, deadline, false};
    if (it != models_.end())
        *it = model;
    else {
        if (models_.size() >= limit_)
            return fail<void>(ErrorCode::resource_unavailable);
        models_.push_back(model);
    }
    return Result<void>::success();
}
Result<void> ModelLoadRegistry::completed(const ModelResidencyKey &key, ResourceOwner owner,
                                          AllocationIdentity allocation, TimePoint now) {
    auto it = std::find_if(models_.begin(), models_.end(),
                           [&](const auto &model) { return model.snapshot.key == key; });
    if (it == models_.end() || it->snapshot.owner != owner || it->cancelled || now >= it->deadline)
        return fail<void>(ErrorCode::state_unavailable);
    if (it->snapshot.state == ModelResidencyState::resident &&
        it->snapshot.allocation == allocation)
        return Result<void>::success();
    if (it->snapshot.state != ModelResidencyState::loading ||
        !resources_.ready_for_use(it->lease, now) ||
        !resources_.compatible(allocation, key.resource, key.host_epoch, key.worker, now))
        return fail<void>(ErrorCode::state_unavailable);
    it->snapshot.state = ModelResidencyState::resident;
    it->snapshot.allocation = allocation;
    return Result<void>::success();
}
Result<void> ModelLoadRegistry::cancel(const ModelResidencyKey &key, ResourceOwner owner) {
    auto it = std::find_if(models_.begin(), models_.end(),
                           [&](const auto &model) { return model.snapshot.key == key; });
    if (it == models_.end() || it->snapshot.owner != owner)
        return fail<void>(ErrorCode::state_unavailable);
    it->cancelled = true;
    if (it->snapshot.state == ModelResidencyState::loading)
        it->snapshot.state = ModelResidencyState::unknown;
    if (it->snapshot.state == ModelResidencyState::resident && it->snapshot.allocation)
        return resources_.drop_reference(*it->snapshot.allocation, owner.job);
    return Result<void>::success();
}
Result<void> ModelLoadRegistry::cleanup_settled(const ModelResidencyKey &key, ResourceOwner owner,
                                                ContainmentProof proof) {
    auto it = std::find_if(models_.begin(), models_.end(),
                           [&](const auto &model) { return model.snapshot.key == key; });
    if (it == models_.end() || it->snapshot.owner != owner || !it->cancelled ||
        (proof != ContainmentProof::worker_quiesced &&
         proof != ContainmentProof::isolated_by_authority))
        return fail<void>(ErrorCode::cleanup_failed);
    if (!resources_.lease_quiescent(it->lease) ||
        (it->snapshot.allocation && resources_.allocation(*it->snapshot.allocation)))
        return fail<void>(ErrorCode::cleanup_failed);
    it->snapshot.state = ModelResidencyState::absent;
    it->snapshot.owner.reset();
    return Result<void>::success();
}
Result<void> ModelLoadRegistry::can_pause(const ModelResidencyKey &key) const {
    auto it = std::find_if(models_.begin(), models_.end(),
                           [&](const auto &model) { return model.snapshot.key == key; });
    if (it != models_.end() && it->snapshot.state == ModelResidencyState::loading &&
        !it->snapshot.pausable)
        return Result<void>::failure(ErrorEnvelope::make(
            ErrorCode::invalid_request, ErrorStage::execution, ExternalOutcome::not_applicable,
            RetryDisposition::prohibited, ErrorReason::unsupported_operation));
    return Result<void>::success();
}
void ModelLoadRegistry::fence(HostEpoch epoch) noexcept {
    for (auto &model : models_)
        if (model.snapshot.key.host_epoch == epoch &&
            model.snapshot.state != ModelResidencyState::absent)
            model.snapshot.state = ModelResidencyState::unknown;
}
std::optional<ModelResidencySnapshot>
ModelLoadRegistry::inspect(const ModelResidencyKey &key) const {
    auto it = std::find_if(models_.begin(), models_.end(),
                           [&](const auto &model) { return model.snapshot.key == key; });
    return it == models_.end() ? std::nullopt : std::optional<ModelResidencySnapshot>{it->snapshot};
}
} // namespace flamoris::runtime
