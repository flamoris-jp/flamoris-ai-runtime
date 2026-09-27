#include "flamoris/runtime/cleanup_observation.hpp"
#include "flamoris/runtime/observation.hpp"
#include <algorithm>
#include <stdexcept>

namespace flamoris::runtime {
ControllerCleanupObservationPort::ControllerCleanupObservationPort(RunObservationLookupPort &lookup,
                                                                   MonotonicClock &clock,
                                                                   std::size_t max_records)
    : lookup_(lookup), clock_(clock), max_records_(max_records) {
    if (!max_records_ || max_records_ > 65536)
        throw std::invalid_argument("invalid cleanup observation bounds");
    records_.reserve(max_records_);
}
Result<CleanupObservationTicket>
ControllerCleanupObservationPort::reserve(RunId run, CleanupId cleanup, TimePoint closes) {
    auto reject = []() {
        return Result<CleanupObservationTicket>::failure(
            ErrorEnvelope::make(ErrorCode::resource_unavailable, ErrorStage::cleanup));
    };
    if (!healthy_ || !cleanup.valid() || closes <= clock_.now() ||
        records_.size() == max_records_ || next_ticket_ == UINT64_MAX ||
        std::any_of(records_.begin(), records_.end(),
                    [&](const auto &r) { return r.cleanup == cleanup; }))
        return reject();
    auto *controller = lookup_.find_observation_run(run);
    if (!controller || controller->id() != run)
        return reject();
    try {
        Record record;
        record.ticket = next_ticket_;
        record.run = run;
        record.cleanup = cleanup;
        record.closes_at = closes;
        LifecycleEvent observed;
        observed.kind = "reconciliation.observed";
        observed.job = controller->root();
        observed.operation = cleanup.value();
        record.observed.events.push_back(std::move(observed));
        LifecycleEvent closed;
        closed.kind = "reconciliation.closed";
        closed.job = controller->root();
        closed.operation = cleanup.value();
        record.closed.events.push_back(std::move(closed));
        auto reservation = controller->observation_store().reserve_reconciliation(1);
        if (!reservation)
            return Result<CleanupObservationTicket>::failure(reservation.error());
        record.reservation = reservation.value();
        records_.push_back(std::move(record));
        return Result<CleanupObservationTicket>::success({next_ticket_++, closes});
    } catch (...) {
        return reject();
    }
}
void ControllerCleanupObservationPort::observed(CleanupObservationTicket ticket, CleanupId cleanup,
                                                LedgerRevision revision) noexcept {
    auto it = std::find_if(records_.begin(), records_.end(), [&](const auto &record) {
        return record.ticket == ticket.value && record.cleanup == cleanup &&
               record.closes_at == ticket.closes_at;
    });
    if (it == records_.end() || it->observed_published || it->closed_published ||
        clock_.now() >= it->closes_at)
        return;
    it->observed_published = true;
    auto *controller = lookup_.find_observation_run(it->run);
    if (!controller || controller->observation_store().state() != ObservationStreamState::open)
        return;
    it->observed.events.front().ledger_revision = revision.value();
    it->observed.events.front().external_outcome = ExternalOutcome::confirmed_success;
    try {
        auto result = controller->commit_observation(
            std::move(it->observed), EventStorageClass::reconciliation, it->reservation);
        if (!result)
            healthy_ = false;
    } catch (...) {
        healthy_ = false;
    }
}
void ControllerCleanupObservationPort::closed(CleanupObservationTicket ticket, CleanupId cleanup,
                                              bool resolved, LedgerRevision revision) noexcept {
    auto it = std::find_if(records_.begin(), records_.end(), [&](const auto &record) {
        return record.ticket == ticket.value && record.cleanup == cleanup &&
               record.closes_at == ticket.closes_at;
    });
    if (it == records_.end() || it->closed_published)
        return;
    it->closed_published = true;
    auto *controller = lookup_.find_observation_run(it->run);
    if (!controller || controller->observation_store().state() != ObservationStreamState::open)
        return;
    it->closed.events.front().ledger_revision = revision.value();
    it->closed.events.front().external_outcome =
        resolved ? ExternalOutcome::confirmed_success : ExternalOutcome::unknown;
    try {
        auto result = controller->commit_observation(
            std::move(it->closed), EventStorageClass::reconciliation, it->reservation);
        if (!result)
            healthy_ = false;
    } catch (...) {
        healthy_ = false;
    }
}
} // namespace flamoris::runtime
