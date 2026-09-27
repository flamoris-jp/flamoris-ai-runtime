#pragma once
#include "flamoris/runtime/resources.hpp"
#include <stdexcept>
#include <vector>

namespace flamoris::runtime::test {
class ManualHost final : public HostAuthorityPort {
  public:
    struct Acquisition {
        ResourceTicket ticket;
        ResourceVector demand;
        TimePoint deadline;
    };
    std::vector<Acquisition> acquisitions;
    std::vector<OperationId> releases, reconciliations;
    std::size_t envelope_requests{};
    bool available{true};
    bool throw_after_acquire{};
    bool throw_reconcile{};
    bool throw_after_release{};
    Result<void> request_envelope(LogicalResourceId, RuntimeInstanceId) override {
        ++envelope_requests;
        return result();
    }
    Result<void> acquire(const ResourceTicket &ticket, const ResourceVector &demand,
                         TimePoint deadline) override {
        if (!available || acquisitions.size() >= 128)
            return Result<void>::failure(
                ErrorEnvelope::make(ErrorCode::native_compute_unavailable));
        acquisitions.push_back({ticket, demand, deadline});
        if (throw_after_acquire)
            throw std::runtime_error("private host diagnostics");
        return Result<void>::success();
    }
    Result<void> release(OperationId operation, const ResourceTicket &, OperationId) override {
        releases.push_back(operation);
        if (throw_after_release)
            throw std::runtime_error("private host diagnostics");
        return result();
    }
    Result<void> reconcile(OperationId operation, const ResourceTicket &, OperationId) override {
        reconciliations.push_back(operation);
        if (throw_reconcile)
            throw std::runtime_error("private host diagnostics");
        return result();
    }

  private:
    Result<void> result() const {
        return available ? Result<void>::success()
                         : Result<void>::failure(
                               ErrorEnvelope::make(ErrorCode::native_compute_unavailable));
    }
};
class ManualCleanupObservation final : public CleanupObservationPort {
  public:
    struct Change {
        CleanupId cleanup;
        bool closed;
        bool resolved;
        LedgerRevision revision;
    };
    std::size_t capacity{128};
    std::size_t reserved{};
    std::vector<Change> changes;
    ManualCleanupObservation() { changes.reserve(256); }
    Result<CleanupObservationTicket> reserve(RunId, CleanupId, TimePoint deadline) override {
        if (reserved >= capacity)
            return Result<CleanupObservationTicket>::failure(
                ErrorEnvelope::make(ErrorCode::resource_unavailable));
        return Result<CleanupObservationTicket>::success({++reserved, deadline});
    }
    void observed(CleanupObservationTicket, CleanupId id,
                  LedgerRevision revision) noexcept override {
        changes.push_back({id, false, false, revision});
    }
    void closed(CleanupObservationTicket, CleanupId id, bool resolved,
                LedgerRevision revision) noexcept override {
        changes.push_back({id, true, resolved, revision});
    }
};
} // namespace flamoris::runtime::test
