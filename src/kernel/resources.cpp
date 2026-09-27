#include "flamoris/runtime/resources.hpp"

#include <algorithm>
#include <limits>

namespace flamoris::runtime {
namespace {
ErrorEnvelope error(ErrorCode code, ErrorStage stage = ErrorStage::admission) {
    return ErrorEnvelope::make(code, stage);
}
template <class T> Result<T> failure(ErrorCode code, ErrorStage stage = ErrorStage::admission) {
    return Result<T>::failure(error(code, stage));
}
bool add(ResourceVector &to, const ResourceVector &by) noexcept {
    ResourceVector result = to;
    for (std::size_t i = 0; i < result.values.size(); ++i) {
        if (by.values[i] > std::numeric_limits<std::uint64_t>::max() - result.values[i])
            return false;
        result.values[i] += by.values[i];
    }
    to = result;
    return true;
}
void add_conservatively(ResourceVector &to, const ResourceVector &by) noexcept {
    if (!add(to, by))
        to.values.fill(std::numeric_limits<std::uint64_t>::max());
}
void subtract(ResourceVector &from, const ResourceVector &by) noexcept {
    for (std::size_t i = 0; i < from.values.size(); ++i)
        from.values[i] -= by.values[i];
}
ResourceVector memory(ResourceVector value) noexcept {
    value[ResourceKind::execution] = 0;
    value[ResourceKind::adapter] = 0;
    return value;
}
ResourceVector execution(ResourceVector value) noexcept {
    ResourceVector result;
    result[ResourceKind::execution] = value[ResourceKind::execution];
    result[ResourceKind::adapter] = value[ResourceKind::adapter];
    return result;
}
bool proof_valid(ContainmentProof proof) noexcept {
    return proof == ContainmentProof::worker_quiesced ||
           proof == ContainmentProof::isolated_by_authority;
}
bool counter_available(std::uint64_t value) noexcept {
    return value != std::numeric_limits<std::uint64_t>::max();
}
} // namespace

bool ResourceVector::fits(const ResourceVector &capacity) const noexcept {
    for (std::size_t i = 0; i < values.size(); ++i)
        if (values[i] > capacity.values[i])
            return false;
    return true;
}
bool ResourceVector::empty() const noexcept {
    return std::all_of(values.begin(), values.end(), [](auto value) { return value == 0; });
}
Result<void> UnavailableHostAuthority::request_envelope(LogicalResourceId, RuntimeInstanceId) {
    return Result<void>::failure(ErrorEnvelope::make(ErrorCode::native_compute_unavailable,
                                                     ErrorStage::admission,
                                                     ExternalOutcome::not_dispatched));
}
Result<void> UnavailableHostAuthority::acquire(const ResourceTicket &, const ResourceVector &,
                                               TimePoint) {
    return Result<void>::failure(ErrorEnvelope::make(ErrorCode::native_compute_unavailable,
                                                     ErrorStage::admission,
                                                     ExternalOutcome::not_dispatched));
}
Result<void> UnavailableHostAuthority::release(OperationId, const ResourceTicket &, OperationId) {
    return Result<void>::failure(ErrorEnvelope::make(ErrorCode::native_compute_unavailable,
                                                     ErrorStage::admission,
                                                     ExternalOutcome::not_dispatched));
}
Result<void> UnavailableHostAuthority::reconcile(OperationId, const ResourceTicket &, OperationId) {
    return Result<void>::failure(ErrorEnvelope::make(ErrorCode::native_compute_unavailable,
                                                     ErrorStage::admission,
                                                     ExternalOutcome::not_dispatched));
}

ResourceManager::ResourceManager(RuntimeInstanceId instance, HostAuthorityPort &host,
                                 CleanupObservationPort &observations, ResourceManagerLimits limits)
    : instance_(instance), host_(host), observations_(observations), limits_(limits) {
    envelopes_.reserve(limits.envelopes);
    reservations_.reserve(limits.reservations);
    allocations_.reserve(limits.allocations);
    cleanup_.reserve(limits.cleanup_records);
}
ResourceManager::EnvelopeRecord *ResourceManager::envelope(LogicalResourceId resource) noexcept {
    auto found = std::find_if(envelopes_.begin(), envelopes_.end(),
                              [&](const auto &e) { return e.value.resource == resource; });
    return found == envelopes_.end() ? nullptr : &*found;
}
const ResourceManager::EnvelopeRecord *
ResourceManager::envelope(LogicalResourceId resource) const noexcept {
    auto found = std::find_if(envelopes_.begin(), envelopes_.end(),
                              [&](const auto &e) { return e.value.resource == resource; });
    return found == envelopes_.end() ? nullptr : &*found;
}
ResourceManager::ReservationRecord *
ResourceManager::reservation(const ResourceTicket &ticket) noexcept {
    auto found = std::find_if(reservations_.begin(), reservations_.end(),
                              [&](const auto &r) { return r.ticket == ticket; });
    return found == reservations_.end() ? nullptr : &*found;
}
const ResourceManager::ReservationRecord *
ResourceManager::reservation(const ResourceTicket &ticket) const noexcept {
    auto found = std::find_if(reservations_.begin(), reservations_.end(),
                              [&](const auto &r) { return r.ticket == ticket; });
    return found == reservations_.end() ? nullptr : &*found;
}
ResourceManager::AllocationRecord *
ResourceManager::find_allocation(AllocationIdentity identity) noexcept {
    auto found = std::find_if(allocations_.begin(), allocations_.end(),
                              [&](const auto &a) { return a.identity == identity; });
    return found == allocations_.end() ? nullptr : &*found;
}
const ResourceManager::AllocationRecord *
ResourceManager::find_allocation(AllocationIdentity identity) const noexcept {
    auto found = std::find_if(allocations_.begin(), allocations_.end(),
                              [&](const auto &a) { return a.identity == identity; });
    return found == allocations_.end() ? nullptr : &*found;
}
bool ResourceManager::fresh(const EnvelopeRecord *e, HostEpoch epoch,
                            TimePoint now) const noexcept {
    return e && !e->fenced && e->value.epoch == epoch && e->value.instance == instance_ &&
           e->value.expires > now && e->value.contract_verified && e->value.reconciled_inventory &&
           e->value.arbitration != HostArbitration::advisory;
}
Result<void> ResourceManager::observe_envelope(const HostEnvelope &value, TimePoint now) {
    auto *current = envelope(value.resource);
    if (value.instance != instance_ ||
        (current && (value.authority_revision < current->value.authority_revision ||
                     value.epoch.value() < current->value.epoch.value())))
        return failure<void>(ErrorCode::state_unavailable);
    if (value.instance != instance_ || !value.epoch.valid() || value.authority_revision == 0 ||
        value.expires <= now || !value.contract_verified || !value.reconciled_inventory ||
        value.arbitration == HostArbitration::advisory) {
        if (current) {
            current->fenced = true;
            ++revision_;
        }
        return failure<void>(ErrorCode::native_compute_unavailable);
    }
    if (current && value.authority_revision == current->value.authority_revision) {
        // An authority revision identifies the complete envelope, not merely a timestamp.
        if (value.epoch != current->value.epoch || value.expires != current->value.expires ||
            value.capacity != current->value.capacity ||
            value.arbitration != current->value.arbitration)
            return failure<void>(ErrorCode::invalid_request);
        return current->fenced ? failure<void>(ErrorCode::state_unavailable)
                               : Result<void>::success();
    }
    if (!current && envelopes_.size() >= limits_.envelopes)
        return failure<void>(ErrorCode::resource_unavailable);
    if (current)
        *current = EnvelopeRecord{value, false};
    else
        envelopes_.push_back({value, false});
    ++revision_;
    return Result<void>::success();
}
void ResourceManager::fence(LogicalResourceId resource) noexcept {
    if (auto *e = envelope(resource); e && !e->fenced) {
        e->fenced = true;
        ++revision_;
    }
}
ResourceVector ResourceManager::run_usage(RunId run) const noexcept {
    ResourceVector total;
    for (const auto &a : allocations_) {
        if (!a.released && std::any_of(a.references.begin(), a.references.end(),
                                       [&](JobId job) { return job.run() == run; }))
            add_conservatively(total, a.footprint);
    }
    // Include the unique shared working set claimed by pending requests, even
    // before those requests become references on the physical allocation.
    for (const auto &a : allocations_) {
        if (a.released || std::any_of(a.references.begin(), a.references.end(),
                                      [&](JobId job) { return job.run() == run; }))
            continue;
        bool required = std::any_of(reservations_.begin(), reservations_.end(), [&](const auto &r) {
            return !r.aborted && !r.quiescent && r.request.owner.job.run() == run &&
                   std::find(r.request.shared.begin(), r.request.shared.end(), a.identity) !=
                       r.request.shared.end();
        });
        if (required)
            add_conservatively(total, a.footprint);
    }
    for (const auto &r : reservations_) {
        if (!r.aborted && r.request.owner.job.run() == run) {
            add_conservatively(total, r.remaining);
            if (r.lease && !r.quiescent)
                add_conservatively(total, execution(r.request.incremental));
        }
    }
    return total;
}
Result<ResourceTicket> ResourceManager::reserve(const ResourceRequest &request, TimePoint now) {
    if (!request.operation.valid() || !request.owner.job.valid() ||
        !request.owner.attempt.valid() || !request.owner.dispatch.valid() ||
        !request.worker.valid() || request.owner.job.run().instance() != instance_ ||
        request.deadline <= now)
        return failure<ResourceTicket>(ErrorCode::invalid_request);
    for (const auto &previous : reservations_) {
        if (previous.request.operation == request.operation) {
            if (previous.request == request && !previous.aborted)
                return Result<ResourceTicket>::success(previous.ticket);
            return failure<ResourceTicket>(ErrorCode::invalid_request);
        }
    }
    const auto *e = envelope(request.resource);
    if (!fresh(e, request.host_epoch, now))
        return failure<ResourceTicket>(ErrorCode::native_compute_unavailable);
    if (reservations_.size() >= limits_.reservations || !counter_available(next_reservation_))
        return failure<ResourceTicket>(ErrorCode::resource_unavailable);
    auto snap = snapshot(request.resource);
    ResourceVector total = snap.resident;
    if (!add(total, snap.reserved) || !add(total, snap.executing) ||
        !add(total, request.incremental) || !total.fits(e->value.capacity))
        return failure<ResourceTicket>(ErrorCode::resource_unavailable);
    ResourceVector run = run_usage(request.owner.job.run());
    if (!add(run, request.incremental))
        return failure<ResourceTicket>(ErrorCode::budget_exceeded);
    std::vector<AllocationIdentity> unique;
    unique.reserve(request.shared.size());
    if (request.shared.size() > limits_.references_per_allocation)
        return failure<ResourceTicket>(ErrorCode::resource_unavailable);
    for (auto identity : request.shared) {
        const auto *a = find_allocation(identity);
        if (!a || a->released || a->release || a->cleanup || a->resource != request.resource ||
            !compatible(identity, request.resource, request.host_epoch, request.worker, now))
            return failure<ResourceTicket>(ErrorCode::state_unavailable);
        if (std::find(unique.begin(), unique.end(), identity) != unique.end())
            return failure<ResourceTicket>(ErrorCode::invalid_request);
        unique.push_back(identity);
        bool in_run = std::any_of(a->references.begin(), a->references.end(),
                                  [&](JobId job) { return job.run() == request.owner.job.run(); });
        bool pending = std::any_of(reservations_.begin(), reservations_.end(), [&](const auto &r) {
            return !r.aborted && !r.quiescent &&
                   r.request.owner.job.run() == request.owner.job.run() &&
                   std::find(r.request.shared.begin(), r.request.shared.end(), identity) !=
                       r.request.shared.end();
        });
        if (!in_run && !pending && !add(run, a->footprint))
            return failure<ResourceTicket>(ErrorCode::budget_exceeded);
        if (a->references.size() >= limits_.references_per_allocation &&
            std::find(a->references.begin(), a->references.end(), request.owner.job) ==
                a->references.end())
            return failure<ResourceTicket>(ErrorCode::resource_unavailable);
    }
    if (!run.fits(request.run_limit))
        return failure<ResourceTicket>(ErrorCode::budget_exceeded);
    ResourceTicket ticket{ReservationId{next_reservation_},
                          request.operation,
                          request.owner,
                          request.resource,
                          request.host_epoch,
                          request.worker};
    ReservationRecord record{
        request, ticket, request.incremental, OperationId{}, HostOutcome::unknown, std::nullopt,
        false,   false};
    // Own the complete liability before publishing the outbound command. A port
    // exception may follow acceptance, so it cannot roll this reservation back.
    reservations_.push_back(std::move(record));
    ++next_reservation_;
    ++revision_;
    try {
        auto queued = host_.acquire(ticket, request.incremental,
                                    request.acquisition_deadline
                                        ? std::min(request.deadline, *request.acquisition_deadline)
                                        : request.deadline);
        if (!queued) {
            if (queued.error().external_outcome() == ExternalOutcome::not_dispatched) {
                reservations_.back().remaining = {};
                reservations_.back().aborted = true;
                ++revision_;
            }
            return Result<ResourceTicket>::failure(queued.error());
        }
    } catch (...) {
        return Result<ResourceTicket>::failure(ErrorEnvelope::make(
            ErrorCode::outcome_unknown, ErrorStage::admission, ExternalOutcome::unknown,
            RetryDisposition::reconciliation_required));
    }
    return Result<ResourceTicket>::success(ticket);
}
std::optional<ResourceTicket> ResourceManager::ticket_for(OperationId operation) const noexcept {
    for (const auto &record : reservations_)
        if (record.ticket.operation == operation && !record.aborted)
            return record.ticket;
    return std::nullopt;
}
Result<void> ResourceManager::acquired(const HostAcquisition &acquisition) {
    auto *r = reservation(acquisition.ticket);
    if (!r || acquisition.instance != instance_ || r->aborted ||
        (acquisition.outcome == HostOutcome::acknowledged && !acquisition.grant.valid()))
        return failure<void>(ErrorCode::state_unavailable);
    if (r->outcome != HostOutcome::unknown) {
        if (r->outcome == acquisition.outcome && r->grant == acquisition.grant)
            return Result<void>::success();
        return failure<void>(ErrorCode::invalid_request);
    }
    r->outcome = acquisition.outcome;
    r->grant = acquisition.grant;
    ++revision_;
    return Result<void>::success();
}
Result<ExecutionLease> ResourceManager::commit_dispatch(const ResourceTicket &ticket,
                                                        const ResourceDispatchGuard &guard,
                                                        TimePoint now) {
    auto *r = reservation(ticket);
    if (!r || r->aborted || r->quiescent || r->quarantined || r->native_stopped ||
        r->host_release || r->lease)
        return failure<ExecutionLease>(ErrorCode::state_unavailable, ErrorStage::dispatch);
    if (!guard.authorized)
        return failure<ExecutionLease>(ErrorCode::permission_denied, ErrorStage::dispatch);
    if (!guard.pins_current)
        return failure<ExecutionLease>(ErrorCode::plan_stale, ErrorStage::dispatch);
    if (!guard.eligible || !guard.state_valid)
        return failure<ExecutionLease>(ErrorCode::state_unavailable, ErrorStage::dispatch);
    if (now >= r->request.deadline ||
        (r->request.acquisition_deadline && now >= *r->request.acquisition_deadline))
        return failure<ExecutionLease>(ErrorCode::job_timeout, ErrorStage::dispatch);
    if (!fresh(envelope(ticket.resource), ticket.host_epoch, now) ||
        r->outcome != HostOutcome::acknowledged)
        return failure<ExecutionLease>(ErrorCode::native_compute_unavailable, ErrorStage::dispatch);
    if (!counter_available(next_lease_))
        return failure<ExecutionLease>(ErrorCode::resource_unavailable);
    for (auto identity : r->request.shared) {
        auto *a = find_allocation(identity);
        if (!a || a->released || a->release || a->cleanup ||
            a->references.size() >= limits_.references_per_allocation)
            return failure<ExecutionLease>(ErrorCode::state_unavailable);
    }
    ExecutionLease lease{LeaseId{next_lease_}, ticket, r->grant};
    for (auto identity : r->request.shared) {
        auto *a = find_allocation(identity);
        if (std::find(a->references.begin(), a->references.end(), ticket.owner.job) ==
            a->references.end())
            a->references.push_back(ticket.owner.job);
    }
    r->remaining = memory(r->remaining);
    r->lease = lease;
    ++next_lease_;
    ++revision_;
    return Result<ExecutionLease>::success(lease);
}
Result<AllocationIdentity> ResourceManager::next_allocation_identity(const ResourceTicket &ticket) {
    auto *r = reservation(ticket);
    if (!r || !r->lease || r->aborted || r->quiescent || r->quarantined || r->native_stopped ||
        r->host_release)
        return failure<AllocationIdentity>(ErrorCode::state_unavailable);
    if (allocations_.size() >= limits_.allocations || !counter_available(next_allocation_))
        return failure<AllocationIdentity>(ErrorCode::resource_unavailable);
    AllocationIdentity identity{instance_, AllocationId{next_allocation_}, ticket.host_epoch,
                                ticket.worker};
    AllocationRecord record{identity,     ticket.reservation, ticket.resource, {},   {},
                            std::nullopt, std::nullopt,       false,           false};
    record.references.reserve(limits_.references_per_allocation);
    allocations_.push_back(std::move(record));
    ++next_allocation_;
    ++revision_;
    return Result<AllocationIdentity>::success(identity);
}
Result<void> ResourceManager::materialized(const ResourceTicket &ticket,
                                           AllocationIdentity identity,
                                           const ResourceVector &footprint) {
    auto *r = reservation(ticket);
    if (!r || !r->lease || r->aborted || r->quiescent || identity.instance != instance_ ||
        identity.host_epoch != ticket.host_epoch || identity.worker != ticket.worker ||
        !identity.allocation.valid() || footprint.empty() || footprint != memory(footprint))
        return failure<void>(ErrorCode::state_unavailable);
    auto *existing = find_allocation(identity);
    if (existing && (existing->origin != ticket.reservation ||
                     existing->resource != ticket.resource || existing->released))
        return failure<void>(ErrorCode::invalid_request);
    if (existing && existing->materialized)
        return existing->footprint == footprint ? Result<void>::success()
                                                : failure<void>(ErrorCode::invalid_request);
    if (!footprint.fits(r->remaining))
        return failure<void>(ErrorCode::resource_unavailable);
    if (!existing) {
        // Direct materialization may use a fresh monotonically issued ID. Workers
        // whose receipts can reorder pre-register with next_allocation_identity.
        if (identity.allocation.value() < next_allocation_ ||
            !counter_available(identity.allocation.value()))
            return failure<void>(ErrorCode::invalid_request);
        if (allocations_.size() >= limits_.allocations)
            return failure<void>(ErrorCode::resource_unavailable);
        AllocationRecord record{identity,     ticket.reservation, ticket.resource, {},   {},
                                std::nullopt, std::nullopt,       false,           false};
        record.references.reserve(limits_.references_per_allocation);
        allocations_.push_back(std::move(record));
        existing = &allocations_.back();
        next_allocation_ = identity.allocation.value() + 1;
    }
    existing->footprint = footprint;
    existing->materialized = true;
    existing->references.push_back(ticket.owner.job);
    subtract(r->remaining, footprint);
    ++revision_;
    return Result<void>::success();
}
Result<void> ResourceManager::materialization_complete(const ResourceTicket &ticket,
                                                       std::span<const AllocationIdentity> manifest,
                                                       TimePoint now) {
    auto *r = reservation(ticket);
    if (!r || !r->lease || !lease_current(*r->lease, now) || manifest.size() > limits_.allocations)
        return failure<void>(ErrorCode::state_unavailable);
    std::vector<AllocationIdentity> canonical(manifest.begin(), manifest.end());
    auto order = [](const auto &a, const auto &b) { return a.allocation < b.allocation; };
    std::sort(canonical.begin(), canonical.end(), order);
    if (std::adjacent_find(canonical.begin(), canonical.end()) != canonical.end())
        return failure<void>(ErrorCode::invalid_request);
    if (r->initial_materialization_complete)
        return r->initial_manifest == canonical ? Result<void>::success()
                                                : failure<void>(ErrorCode::invalid_request);
    std::size_t present = 0;
    for (const auto &a : allocations_)
        if (a.origin == ticket.reservation && !a.released) {
            if (!a.materialized ||
                std::find(canonical.begin(), canonical.end(), a.identity) == canonical.end())
                return failure<void>(ErrorCode::state_unavailable);
            ++present;
        }
    if (present != canonical.size())
        return failure<void>(ErrorCode::state_unavailable);
    r->initial_manifest = std::move(canonical);
    r->initial_materialization_complete = true;
    ++revision_;
    return Result<void>::success();
}
Result<void> ResourceManager::request_host_release(const ResourceTicket &ticket,
                                                   OperationId operation) {
    auto *r = reservation(ticket);
    if (!r || !operation.valid() || !r->grant.valid() || r->outcome != HostOutcome::acknowledged)
        return failure<void>(ErrorCode::state_unavailable, ErrorStage::cleanup);
    if (r->host_release) {
        if (*r->host_release != operation)
            return failure<void>(ErrorCode::invalid_request, ErrorStage::cleanup);
        return Result<void>::success();
    }
    r->host_release = operation;
    ++revision_;
    try {
        auto sent = host_.release(operation, ticket, r->grant);
        if (!sent) {
            if (sent.error().external_outcome() == ExternalOutcome::not_dispatched)
                r->host_release.reset();
            return sent;
        }
    } catch (...) {
        return Result<void>::failure(ErrorEnvelope::make(
            ErrorCode::outcome_unknown, ErrorStage::cleanup, ExternalOutcome::unknown,
            RetryDisposition::reconciliation_required));
    }
    return Result<void>::success();
}
std::optional<OperationId>
ResourceManager::host_release_operation(const ResourceTicket &ticket) const noexcept {
    const auto *r = reservation(ticket);
    return r ? r->host_release : std::nullopt;
}
bool ResourceManager::reservation_settled(const ResourceTicket &ticket) const noexcept {
    const auto *r = reservation(ticket);
    return r && (r->aborted || r->quiescent);
}
void ResourceManager::settle_execution(ReservationRecord &r, TimePoint now) noexcept {
    if (!r.native_stopped || !r.host_released || (!r.remaining.empty() && !r.unmaterialized_absent))
        return;
    if (r.quiescent || r.aborted)
        return;
    r.remaining = {};
    if (r.lease)
        r.quiescent = true;
    else
        r.aborted = true;
    if (r.unmaterialized_absent)
        for (auto &a : allocations_)
            if (a.origin == r.ticket.reservation && !a.materialized)
                a.released = true;
    ++revision_;
    reconcile_cleanup(now);
}
Result<void> ResourceManager::observe_host_release(const HostReleaseReceipt &receipt,
                                                   TimePoint now) {
    auto *r = reservation(receipt.ticket);
    if (!r || r->host_release != receipt.operation || r->grant != receipt.grant)
        return failure<void>(ErrorCode::state_unavailable, ErrorStage::cleanup);
    if (receipt.outcome != HostOutcome::acknowledged)
        return Result<void>::failure(ErrorEnvelope::make(
            ErrorCode::outcome_unknown, ErrorStage::cleanup, ExternalOutcome::unknown,
            RetryDisposition::reconciliation_required));
    if (!r->host_released) {
        r->host_released = true;
        ++revision_;
    }
    settle_execution(*r, now);
    return Result<void>::success();
}
Result<void> ResourceManager::native_quiesced(const ResourceTicket &ticket, ContainmentProof proof,
                                              bool unmaterialized_absent_confirmed, TimePoint now) {
    auto *r = reservation(ticket);
    if (!r || !proof_valid(proof))
        return failure<void>(ErrorCode::cleanup_failed, ErrorStage::cleanup);
    r->native_stopped = true;
    r->unmaterialized_absent = r->unmaterialized_absent || unmaterialized_absent_confirmed;
    ++revision_;
    settle_execution(*r, now);
    return Result<void>::success();
}
bool ResourceManager::lease_quiescent(const ExecutionLease &lease) const noexcept {
    auto *r = reservation(lease.ticket);
    return r && r->lease == lease && r->quiescent;
}
bool ResourceManager::lease_current(const ExecutionLease &lease, TimePoint now) const noexcept {
    auto *r = reservation(lease.ticket);
    return r && r->lease == lease && !r->aborted && !r->quiescent && !r->quarantined &&
           !r->native_stopped && !r->host_release && now < r->request.deadline &&
           fresh(envelope(lease.ticket.resource), lease.ticket.host_epoch, now);
}
bool ResourceManager::ready_for_use(const ExecutionLease &lease, TimePoint now) const noexcept {
    auto *r = reservation(lease.ticket);
    return r && r->lease == lease && !r->aborted && !r->quiescent && !r->quarantined &&
           !r->native_stopped && !r->host_release &&
           (r->remaining.empty() || r->initial_materialization_complete) &&
           now < r->request.deadline &&
           fresh(envelope(lease.ticket.resource), lease.ticket.host_epoch, now);
}
Result<void> ResourceManager::abort_preparation(const ResourceTicket &ticket,
                                                bool proven_no_external_acquisition,
                                                TimePoint now) {
    auto *r = reservation(ticket);
    if (!r || r->lease)
        return failure<void>(ErrorCode::state_unavailable);
    if (r->aborted)
        return Result<void>::success();
    if (!proven_no_external_acquisition && r->outcome != HostOutcome::rejected) {
        if (r->reconciliation_requested)
            return failure<void>(ErrorCode::outcome_unknown, ErrorStage::cleanup);
        r->reconciliation_requested = true;
        try {
            auto result = host_.reconcile(ticket.operation, ticket, r->grant);
            if (!result) {
                if (result.error().external_outcome() == ExternalOutcome::not_dispatched)
                    r->reconciliation_requested = false;
                return result;
            }
        } catch (...) {
            return Result<void>::failure(ErrorEnvelope::make(
                ErrorCode::outcome_unknown, ErrorStage::cleanup, ExternalOutcome::unknown,
                RetryDisposition::reconciliation_required));
        }
        return failure<void>(ErrorCode::outcome_unknown, ErrorStage::cleanup);
    }
    r->remaining = {};
    r->aborted = true;
    ++revision_;
    reconcile_cleanup(now);
    return Result<void>::success();
}
Result<void> ResourceManager::quiesced(const QuiescenceEvidence &evidence, TimePoint now) {
    auto *r = reservation(evidence.lease.ticket);
    if (!r || r->lease != evidence.lease)
        return failure<void>(ErrorCode::state_unavailable, ErrorStage::cleanup);
    if (r->quiescent)
        return Result<void>::success();
    if (!proof_valid(evidence.proof) || !evidence.host_release_confirmed)
        return failure<void>(ErrorCode::cleanup_failed, ErrorStage::cleanup);
    // Native quiescence does not prove an uncertain partial allocation absent.
    if (!r->remaining.empty() && !evidence.unmaterialized_absent_confirmed)
        return failure<void>(ErrorCode::cleanup_failed, ErrorStage::cleanup);
    r->remaining = {};
    r->quiescent = true;
    r->native_stopped = true;
    r->host_released = true;
    r->unmaterialized_absent = evidence.unmaterialized_absent_confirmed;
    if (evidence.unmaterialized_absent_confirmed) {
        for (auto &a : allocations_)
            if (a.origin == r->ticket.reservation && !a.materialized)
                a.released = true;
    }
    ++revision_;
    reconcile_cleanup(now);
    return Result<void>::success();
}
Result<void> ResourceManager::add_reference(AllocationIdentity identity, JobId job,
                                            const ResourceVector &run_limit) {
    auto *a = find_allocation(identity);
    if (!a || !a->materialized || a->released || a->release || a->cleanup ||
        job.run().instance() != instance_)
        return failure<void>(ErrorCode::state_unavailable);
    if (std::find(a->references.begin(), a->references.end(), job) != a->references.end())
        return Result<void>::success();
    if (a->references.size() >= limits_.references_per_allocation)
        return failure<void>(ErrorCode::resource_unavailable);
    auto usage = run_usage(job.run());
    const bool shared_pending =
        std::any_of(reservations_.begin(), reservations_.end(), [&](const auto &r) {
            return !r.aborted && !r.quiescent && r.request.owner.job.run() == job.run() &&
                   std::find(r.request.shared.begin(), r.request.shared.end(), identity) !=
                       r.request.shared.end();
        });
    if (!shared_pending &&
        !std::any_of(a->references.begin(), a->references.end(),
                     [&](JobId ref) { return ref.run() == job.run(); }) &&
        !add(usage, a->footprint))
        return failure<void>(ErrorCode::budget_exceeded);
    if (!usage.fits(run_limit))
        return failure<void>(ErrorCode::budget_exceeded);
    a->references.push_back(job);
    ++revision_;
    return Result<void>::success();
}
Result<void> ResourceManager::drop_reference(AllocationIdentity identity, JobId job) {
    auto *a = find_allocation(identity);
    if (!a)
        return failure<void>(ErrorCode::state_unavailable);
    auto found = std::find(a->references.begin(), a->references.end(), job);
    if (found != a->references.end()) {
        a->references.erase(found);
        ++revision_;
    }
    return Result<void>::success();
}
Result<void> ResourceManager::request_release(AllocationIdentity identity, OperationId operation) {
    auto *a = find_allocation(identity);
    if (!a || !operation.valid())
        return failure<void>(ErrorCode::state_unavailable, ErrorStage::cleanup);
    if (a->release)
        return *a->release == operation ? Result<void>::success()
                                        : failure<void>(ErrorCode::invalid_request);
    if (!a->references.empty())
        return failure<void>(ErrorCode::cleanup_failed, ErrorStage::cleanup);
    a->release = operation;
    ++revision_;
    return Result<void>::success();
}
Result<bool> ResourceManager::release_confirmed(const ReleaseEvidence &evidence, TimePoint now) {
    auto *a = find_allocation(evidence.allocation);
    if (!a || !a->release || *a->release != evidence.release_operation ||
        !evidence.physical_release_confirmed)
        return failure<bool>(ErrorCode::state_unavailable, ErrorStage::cleanup);
    if (a->released)
        return Result<bool>::success(false);
    a->released = true;
    ++revision_;
    reconcile_cleanup(now);
    return Result<bool>::success(true);
}
Result<CleanupId> ResourceManager::transfer_to_cleanup(const CleanupTransfer &transfer,
                                                       TimePoint now) {
    auto *a = find_allocation(transfer.allocation);
    if (!a || a->released || !transfer.release_operation.valid())
        return failure<CleanupId>(ErrorCode::state_unavailable, ErrorStage::cleanup);
    if (a->cleanup) {
        auto found = std::find_if(cleanup_.begin(), cleanup_.end(),
                                  [&](const auto &c) { return c.id == *a->cleanup; });
        return found != cleanup_.end() && found->operation == transfer.release_operation &&
                       found->lease == transfer.lease
                   ? Result<CleanupId>::success(*a->cleanup)
                   : failure<CleanupId>(ErrorCode::invalid_request);
    }
    if (!proof_valid(transfer.containment) || transfer.observation_deadline <= now ||
        !transfer.lease || !a->references.empty())
        return failure<CleanupId>(ErrorCode::cleanup_failed, ErrorStage::cleanup);
    auto *r = reservation(transfer.lease->ticket);
    if (!r || r->lease != transfer.lease || r->ticket.resource != a->resource ||
        r->ticket.host_epoch != a->identity.host_epoch || r->ticket.worker != a->identity.worker)
        return failure<CleanupId>(ErrorCode::state_unavailable, ErrorStage::cleanup);
    if (cleanup_.size() >= limits_.cleanup_records || !counter_available(next_cleanup_))
        return failure<CleanupId>(ErrorCode::resource_unavailable, ErrorStage::cleanup);
    if (!a->release || *a->release != transfer.release_operation)
        return failure<CleanupId>(ErrorCode::state_unavailable, ErrorStage::cleanup);
    // Maintenance is an explicit finite part of the originally admitted vector.
    if (!transfer.maintenance_allowance.fits(r->request.incremental))
        return failure<CleanupId>(ErrorCode::resource_unavailable, ErrorStage::cleanup);
    const CleanupId id{next_cleanup_};
    auto receipt =
        Result<CleanupObservationTicket>::failure(error(ErrorCode::resource_unavailable));
    try {
        receipt =
            observations_.reserve(r->ticket.owner.job.run(), id, transfer.observation_deadline);
    } catch (...) {
        return failure<CleanupId>(ErrorCode::resource_unavailable, ErrorStage::cleanup);
    }
    if (!receipt)
        return Result<CleanupId>::failure(receipt.error());
    if (receipt.value().value == 0 || receipt.value().closes_at != transfer.observation_deadline)
        return failure<CleanupId>(ErrorCode::cleanup_failed, ErrorStage::cleanup);
    cleanup_.push_back({id, a->identity, r->ticket, transfer.lease, transfer.release_operation,
                        receipt.value(), transfer.maintenance_allowance, false, false});
    a->cleanup = id;
    r->quarantined = true;
    ++next_cleanup_;
    ++revision_;
    return Result<CleanupId>::success(id);
}
Result<CleanupId> ResourceManager::transfer_acquisition_to_cleanup(const ResourceTicket &ticket,
                                                                   ContainmentProof containment,
                                                                   TimePoint observation_deadline,
                                                                   TimePoint now) {
    auto *r = reservation(ticket);
    if (!r || r->aborted || r->quiescent || !proof_valid(containment) ||
        observation_deadline <= now)
        return failure<CleanupId>(ErrorCode::cleanup_failed, ErrorStage::cleanup);
    for (const auto &record : cleanup_)
        if (!record.allocation && record.ticket == ticket)
            return Result<CleanupId>::success(record.id);
    if (std::any_of(allocations_.begin(), allocations_.end(),
                    [&](const auto &a) { return a.origin == ticket.reservation && !a.released; }))
        return failure<CleanupId>(ErrorCode::cleanup_failed, ErrorStage::cleanup);
    if (cleanup_.size() >= limits_.cleanup_records || !counter_available(next_cleanup_))
        return failure<CleanupId>(ErrorCode::resource_unavailable, ErrorStage::cleanup);
    CleanupId id{next_cleanup_};
    auto receipt =
        Result<CleanupObservationTicket>::failure(error(ErrorCode::resource_unavailable));
    try {
        receipt = observations_.reserve(ticket.owner.job.run(), id, observation_deadline);
    } catch (...) {
        return failure<CleanupId>(ErrorCode::resource_unavailable, ErrorStage::cleanup);
    }
    if (!receipt)
        return Result<CleanupId>::failure(receipt.error());
    if (receipt.value().value == 0 || receipt.value().closes_at != observation_deadline)
        return failure<CleanupId>(ErrorCode::cleanup_failed, ErrorStage::cleanup);
    cleanup_.push_back(
        {id, std::nullopt, ticket, r->lease, ticket.operation, receipt.value(), {}, false, false});
    r->quarantined = true;
    ++next_cleanup_;
    ++revision_;
    return Result<CleanupId>::success(id);
}
void ResourceManager::reconcile_cleanup(TimePoint now) noexcept {
    close_observations(now);
    for (auto &c : cleanup_)
        if (!c.resolved) {
            const auto *a = c.allocation ? find_allocation(*c.allocation) : nullptr;
            const auto *r = reservation(c.ticket);
            if (c.allocation) {
                if (!a || !a->released || (c.lease && (!r || !r->quiescent)))
                    continue;
            } else {
                if (!r || !(r->aborted || (r->quiescent && r->remaining.empty())))
                    continue;
                if (std::any_of(allocations_.begin(), allocations_.end(),
                                [&](const auto &allocation) {
                                    return allocation.origin == c.ticket.reservation &&
                                           !allocation.released;
                                }))
                    continue;
            }
            c.resolved = true;
            if (!c.closed) {
                observations_.observed(c.observation, c.id, LedgerRevision{revision_});
                observations_.closed(c.observation, c.id, true, LedgerRevision{revision_});
                c.closed = true;
            }
        }
}
void ResourceManager::close_observations(TimePoint now) noexcept {
    for (auto &c : cleanup_)
        if (!c.closed && now >= c.observation.closes_at) {
            observations_.closed(c.observation, c.id, c.resolved, LedgerRevision{revision_});
            c.closed = true;
        }
}
bool ResourceManager::compatible(AllocationIdentity identity, LogicalResourceId resource,
                                 HostEpoch epoch, NativeWorkerGeneration worker,
                                 TimePoint now) const noexcept {
    const auto *a = find_allocation(identity);
    return a && a->materialized && !a->released && !a->release && !a->cleanup &&
           a->resource == resource && identity.instance == instance_ &&
           identity.host_epoch == epoch && identity.worker == worker &&
           fresh(envelope(resource), epoch, now);
}
Result<void> ResourceManager::dependency_feasible(LogicalResourceId resource,
                                                  const ResourceVector &child,
                                                  const ResourceVector &retained_parent) const {
    const auto *e = envelope(resource);
    if (!e || e->fenced)
        return failure<void>(ErrorCode::native_compute_unavailable);
    auto demand = child;
    if (!add(demand, retained_parent) || !demand.fits(e->value.capacity)) {
        auto eout = ErrorEnvelope::make(
            ErrorCode::resource_unavailable, ErrorStage::admission, ExternalOutcome::not_applicable,
            RetryDisposition::prohibited, ErrorReason::resource_deadlock);
        return Result<void>::failure(std::move(eout));
    }
    return Result<void>::success();
}
ResourceSnapshot ResourceManager::snapshot(LogicalResourceId resource) const noexcept {
    ResourceSnapshot result{LedgerRevision{revision_}, {}, {}, {}, {}, 0, 0};
    for (const auto &a : allocations_)
        if (a.resource == resource && !a.released) {
            add(result.resident, a.footprint);
            ++result.allocations;
            if (a.cleanup)
                add(result.quarantine, a.footprint);
        }
    for (const auto &r : reservations_)
        if (r.ticket.resource == resource && !r.aborted) {
            add(result.reserved, r.remaining);
            if (r.lease && !r.quiescent)
                add(result.executing, execution(r.request.incremental));
        }
    for (const auto &c : cleanup_)
        if (!c.resolved) {
            if (c.ticket.resource == resource) {
                ++result.cleanup_records;
                if (!c.allocation) {
                    const auto *r = reservation(c.ticket);
                    if (r) {
                        add_conservatively(result.quarantine, r->remaining);
                        if (r->lease && !r->quiescent)
                            add_conservatively(result.quarantine,
                                               execution(r->request.incremental));
                    }
                }
            }
        }
    return result;
}
std::optional<AllocationSnapshot> ResourceManager::allocation(AllocationIdentity identity) const {
    const auto *a = find_allocation(identity);
    if (!a || a->released || !a->materialized)
        return std::nullopt;
    return AllocationSnapshot{a->identity,
                              a->resource,
                              a->footprint,
                              a->references.size(),
                              a->release.has_value(),
                              a->cleanup.has_value()};
}
std::size_t ResourceManager::cleanup_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(cleanup_.begin(), cleanup_.end(), [](const auto &c) { return !c.resolved; }));
}
std::size_t ResourceManager::retire_settled() noexcept {
    std::size_t retired = 0;
    auto old_cleanup = cleanup_.size();
    std::erase_if(cleanup_, [](const auto &record) { return record.resolved && record.closed; });
    retired += old_cleanup - cleanup_.size();
    auto old_allocations = allocations_.size();
    std::erase_if(allocations_, [&](const auto &record) {
        return record.released &&
               (!record.cleanup ||
                std::none_of(cleanup_.begin(), cleanup_.end(),
                             [&](const auto &c) { return c.id == *record.cleanup; }));
    });
    retired += old_allocations - allocations_.size();
    auto old_reservations = reservations_.size();
    std::erase_if(reservations_, [&](const auto &record) {
        return (record.aborted || record.quiescent) &&
               std::none_of(allocations_.begin(), allocations_.end(),
                            [&](const auto &a) { return a.origin == record.ticket.reservation; }) &&
               std::none_of(cleanup_.begin(), cleanup_.end(),
                            [&](const auto &c) { return c.ticket == record.ticket; });
    });
    retired += old_reservations - reservations_.size();
    if (retired != 0)
        ++revision_;
    return retired;
}
} // namespace flamoris::runtime
