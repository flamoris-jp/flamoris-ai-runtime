#pragma once

#include "flamoris/runtime/clock.hpp"
#include "flamoris/runtime/ids.hpp"
#include "flamoris/runtime/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace flamoris::runtime {

// The control executor is the sole owner of ResourceManager. Ports enqueue value
// commands; they must not perform blocking host I/O on that executor.
enum class ResourceKind : std::size_t {
    ram,
    device,
    transfer,
    execution,
    adapter,
    result,
    control,
    count
};
struct ResourceVector {
    std::array<std::uint64_t, static_cast<std::size_t>(ResourceKind::count)> values{};
    std::uint64_t operator[](ResourceKind kind) const noexcept {
        return values[static_cast<std::size_t>(kind)];
    }
    std::uint64_t &operator[](ResourceKind kind) noexcept {
        return values[static_cast<std::size_t>(kind)];
    }
    bool operator==(const ResourceVector &) const = default;
    bool fits(const ResourceVector &capacity) const noexcept;
    bool empty() const noexcept;
};
struct LogicalResourceId {
    std::uint64_t value{};
    auto operator<=>(const LogicalResourceId &) const = default;
};
struct AllocationIdentity {
    RuntimeInstanceId instance;
    AllocationId allocation;
    HostEpoch host_epoch;
    NativeWorkerGeneration worker;
    bool operator==(const AllocationIdentity &) const = default;
};
struct ResourceOwner {
    JobId job;
    AttemptId attempt;
    DispatchGeneration dispatch;
    bool operator==(const ResourceOwner &) const = default;
};
enum class HostArbitration { advisory, enforced_generation, enforced_single_owner };
struct HostEnvelope {
    LogicalResourceId resource;
    RuntimeInstanceId instance;
    HostEpoch epoch;
    std::uint64_t authority_revision{};
    TimePoint expires{};
    ResourceVector capacity;
    HostArbitration arbitration{HostArbitration::advisory};
    bool contract_verified{};
    // Explicit host evidence that any previous incarnation's obligations have
    // been included in this envelope or reconciled. A fresh local ledger is not proof.
    bool reconciled_inventory{};
};
struct ResourceRequest {
    OperationId operation;
    ResourceOwner owner;
    LogicalResourceId resource;
    HostEpoch host_epoch;
    NativeWorkerGeneration worker;
    ResourceVector incremental;
    ResourceVector run_limit;
    TimePoint deadline{};
    std::vector<AllocationIdentity> shared;
    std::optional<TimePoint> acquisition_deadline{};
    bool operator==(const ResourceRequest &) const = default;
};
struct ResourceTicket {
    ReservationId reservation;
    OperationId operation;
    ResourceOwner owner;
    LogicalResourceId resource;
    HostEpoch host_epoch;
    NativeWorkerGeneration worker;
    bool operator==(const ResourceTicket &) const = default;
};
enum class HostOutcome { acknowledged, rejected, unknown };
struct HostAcquisition {
    ResourceTicket ticket;
    RuntimeInstanceId instance;
    OperationId grant;
    HostOutcome outcome{HostOutcome::unknown};
};
struct HostReleaseReceipt {
    OperationId operation;
    ResourceTicket ticket;
    OperationId grant;
    HostOutcome outcome{HostOutcome::acknowledged};
};
class HostAuthorityPort {
  public:
    virtual ~HostAuthorityPort() = default;
    virtual Result<void> request_envelope(LogicalResourceId, RuntimeInstanceId) = 0;
    virtual Result<void> acquire(const ResourceTicket &, const ResourceVector &,
                                 TimePoint deadline) = 0;
    virtual Result<void> release(OperationId, const ResourceTicket &, OperationId grant) = 0;
    virtual Result<void> reconcile(OperationId, const ResourceTicket &, OperationId grant) = 0;
};
class UnavailableHostAuthority final : public HostAuthorityPort {
  public:
    Result<void> request_envelope(LogicalResourceId, RuntimeInstanceId) override;
    Result<void> acquire(const ResourceTicket &, const ResourceVector &, TimePoint) override;
    Result<void> release(OperationId, const ResourceTicket &, OperationId) override;
    Result<void> reconcile(OperationId, const ResourceTicket &, OperationId) override;
};
struct ResourceDispatchGuard {
    bool eligible{};
    bool authorized{};
    bool pins_current{};
    bool state_valid{};
};
struct ExecutionLease {
    LeaseId id;
    ResourceTicket ticket;
    OperationId host_grant;
    bool operator==(const ExecutionLease &) const = default;
};
enum class ContainmentProof { none, callback_fenced, worker_quiesced, isolated_by_authority };
struct QuiescenceEvidence {
    ExecutionLease lease;
    ContainmentProof proof{ContainmentProof::none};
    bool host_release_confirmed{};
    bool unmaterialized_absent_confirmed{};
};
struct ReleaseEvidence {
    AllocationIdentity allocation;
    OperationId release_operation;
    bool physical_release_confirmed{};
};
struct CleanupObservationTicket {
    std::uint64_t value{};
    TimePoint closes_at{};
};
// A port reserves a bounded post-terminal observed + closed group before debt
// transfer. Calls after reserve cannot fail or mutate lifecycle/terminal intent.
class CleanupObservationPort {
  public:
    virtual ~CleanupObservationPort() = default;
    virtual Result<CleanupObservationTicket> reserve(RunId, CleanupId, TimePoint) = 0;
    virtual void observed(CleanupObservationTicket, CleanupId, LedgerRevision) noexcept = 0;
    virtual void closed(CleanupObservationTicket, CleanupId, bool resolved,
                        LedgerRevision) noexcept = 0;
};
struct CleanupTransfer {
    AllocationIdentity allocation;
    std::optional<ExecutionLease> lease;
    OperationId release_operation;
    ContainmentProof containment{ContainmentProof::none};
    ResourceVector maintenance_allowance;
    TimePoint observation_deadline{};
};
struct AllocationSnapshot {
    AllocationIdentity identity;
    LogicalResourceId resource;
    ResourceVector footprint;
    std::size_t references{};
    bool release_requested{};
    bool quarantined{};
};
struct ResourceSnapshot {
    LedgerRevision revision;
    ResourceVector resident;
    ResourceVector reserved;
    ResourceVector executing;
    ResourceVector quarantine;
    std::size_t cleanup_records{};
    std::size_t allocations{};
};
struct ResourceManagerLimits {
    std::size_t envelopes{16};
    std::size_t reservations{1024};
    std::size_t allocations{1024};
    std::size_t references_per_allocation{256};
    std::size_t cleanup_records{128};
};
class ResourceManager {
  public:
    ResourceManager(RuntimeInstanceId, HostAuthorityPort &, CleanupObservationPort &,
                    ResourceManagerLimits = {});
    Result<void> observe_envelope(const HostEnvelope &, TimePoint now);
    void fence(LogicalResourceId) noexcept;
    Result<ResourceTicket> reserve(const ResourceRequest &, TimePoint now);
    std::optional<ResourceTicket> ticket_for(OperationId) const noexcept;
    Result<void> acquired(const HostAcquisition &);
    Result<ExecutionLease> commit_dispatch(const ResourceTicket &, const ResourceDispatchGuard &,
                                           TimePoint now);
    Result<AllocationIdentity> next_allocation_identity(const ResourceTicket &);
    Result<void> materialized(const ResourceTicket &, AllocationIdentity, const ResourceVector &);
    // The native owner reports the complete initial allocation manifest. Any unused
    // admitted growth/scratch headroom remains reserved until genuine quiescence.
    Result<void> materialization_complete(const ResourceTicket &,
                                          std::span<const AllocationIdentity>, TimePoint now);
    Result<void> request_host_release(const ResourceTicket &, OperationId);
    Result<void> observe_host_release(const HostReleaseReceipt &, TimePoint now);
    Result<void> native_quiesced(const ResourceTicket &, ContainmentProof,
                                 bool unmaterialized_absent_confirmed, TimePoint now);
    std::optional<OperationId> host_release_operation(const ResourceTicket &) const noexcept;
    bool reservation_settled(const ResourceTicket &) const noexcept;
    bool lease_quiescent(const ExecutionLease &) const noexcept;
    bool lease_current(const ExecutionLease &, TimePoint now) const noexcept;
    bool ready_for_use(const ExecutionLease &, TimePoint now) const noexcept;
    Result<void> abort_preparation(const ResourceTicket &, bool proven_no_external_acquisition,
                                   TimePoint now = {});
    Result<void> quiesced(const QuiescenceEvidence &, TimePoint now = {});
    Result<void> add_reference(AllocationIdentity, JobId, const ResourceVector &run_limit);
    Result<void> drop_reference(AllocationIdentity, JobId);
    Result<void> request_release(AllocationIdentity, OperationId);
    Result<bool> release_confirmed(const ReleaseEvidence &, TimePoint now);
    Result<CleanupId> transfer_to_cleanup(const CleanupTransfer &, TimePoint now);
    Result<CleanupId> transfer_acquisition_to_cleanup(const ResourceTicket &, ContainmentProof,
                                                      TimePoint observation_deadline,
                                                      TimePoint now);
    void close_observations(TimePoint now) noexcept;
    bool compatible(AllocationIdentity, LogicalResourceId, HostEpoch, NativeWorkerGeneration,
                    TimePoint now) const noexcept;
    Result<void> dependency_feasible(LogicalResourceId, const ResourceVector &child,
                                     const ResourceVector &retained_parent) const;
    ResourceSnapshot snapshot(LogicalResourceId) const noexcept;
    std::optional<AllocationSnapshot> allocation(AllocationIdentity) const;
    std::size_t cleanup_count() const noexcept;
    // Reclaims only fully acknowledged bookkeeping; never expires uncertainty.
    std::size_t retire_settled() noexcept;

  private:
    struct EnvelopeRecord {
        HostEnvelope value;
        bool fenced{};
    };
    struct ReservationRecord {
        ResourceRequest request;
        ResourceTicket ticket;
        ResourceVector remaining;
        OperationId grant;
        HostOutcome outcome{HostOutcome::unknown};
        std::optional<ExecutionLease> lease;
        bool quiescent{};
        bool aborted{};
        bool quarantined{};
        bool initial_materialization_complete{};
        std::vector<AllocationIdentity> initial_manifest{};
        std::optional<OperationId> host_release{};
        bool host_released{};
        bool native_stopped{};
        bool unmaterialized_absent{};
        bool reconciliation_requested{};
    };
    struct AllocationRecord {
        AllocationIdentity identity;
        ReservationId origin;
        LogicalResourceId resource;
        ResourceVector footprint;
        std::vector<JobId> references;
        std::optional<OperationId> release;
        std::optional<CleanupId> cleanup;
        bool released{};
        bool materialized{};
    };
    struct CleanupRecord {
        CleanupId id;
        std::optional<AllocationIdentity> allocation;
        ResourceTicket ticket;
        std::optional<ExecutionLease> lease;
        OperationId operation;
        CleanupObservationTicket observation;
        ResourceVector maintenance;
        bool resolved{};
        bool closed{};
    };
    EnvelopeRecord *envelope(LogicalResourceId) noexcept;
    const EnvelopeRecord *envelope(LogicalResourceId) const noexcept;
    ReservationRecord *reservation(const ResourceTicket &) noexcept;
    const ReservationRecord *reservation(const ResourceTicket &) const noexcept;
    AllocationRecord *find_allocation(AllocationIdentity) noexcept;
    const AllocationRecord *find_allocation(AllocationIdentity) const noexcept;
    ResourceVector run_usage(RunId) const noexcept;
    void reconcile_cleanup(TimePoint now) noexcept;
    void settle_execution(ReservationRecord &, TimePoint now) noexcept;
    bool fresh(const EnvelopeRecord *, HostEpoch, TimePoint) const noexcept;
    RuntimeInstanceId instance_;
    HostAuthorityPort &host_;
    CleanupObservationPort &observations_;
    ResourceManagerLimits limits_;
    std::vector<EnvelopeRecord> envelopes_;
    std::vector<ReservationRecord> reservations_;
    std::vector<AllocationRecord> allocations_;
    std::vector<CleanupRecord> cleanup_;
    std::uint64_t next_reservation_{1}, next_lease_{1}, next_cleanup_{1}, next_allocation_{1},
        revision_{0};
};

// Move-only payload descriptor. It has no scheduler identity or execution lease.
struct StateReference {
    StateReferenceId id;
    JobId owner;
    std::uint64_t compatibility_revision{};
    std::vector<AllocationIdentity> allocations;
    StateReference() = default;
    StateReference(const StateReference &) = delete;
    StateReference &operator=(const StateReference &) = delete;
    StateReference(StateReference &&) noexcept = default;
    StateReference &operator=(StateReference &&) noexcept = default;
};
} // namespace flamoris::runtime
