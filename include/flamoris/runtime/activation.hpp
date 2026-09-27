#pragma once

#include "flamoris/runtime/resources.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace flamoris::runtime {

// Process ownership, starts, compatible waiter dedup and startup cleanup belong
// to the external gateway. This client only observes generation-bound receipts.
struct ActivationKey {
    std::string service;
    std::uint64_t configuration_revision{};
    std::uint64_t ownership_scope{};
    bool operator==(const ActivationKey &) const = default;
};
enum class ActivationScope { activate, inspect, stop, model };
class ActivationPolicyPort {
  public:
    virtual ~ActivationPolicyPort() = default;
    virtual Result<void> authorize(ActivationScope, const ActivationKey &) = 0;
};
struct ActivationRequest {
    OperationId request;
    ActivationKey key;
    HostEpoch expected_epoch;
    TimePoint deadline{};
};
enum class ProcessStatus {
    requested,
    starting,
    ready,
    draining,
    stopping,
    stopped,
    failed,
    unknown
};
struct ActivationObservation {
    OperationId request;
    ActivationKey key;
    ActivationGeneration generation;
    HostEpoch host_epoch;
    ProcessStatus status{ProcessStatus::unknown};
    RuntimeInstanceId instance;
    std::uint64_t admission_epoch{};
    bool safe_stop_confirmed{};
    bool operator==(const ActivationObservation &) const = default;
};
class HostActivationPort {
  public:
    virtual ~HostActivationPort() = default;
    virtual Result<void> request(const ActivationRequest &) = 0;
    virtual Result<void> detach_waiter(OperationId, const ActivationKey &) = 0;
    virtual Result<void> request_stop(const ActivationObservation &, TimePoint deadline) = 0;
};
class UnavailableActivationGateway final : public HostActivationPort {
  public:
    Result<void> request(const ActivationRequest &) override;
    Result<void> detach_waiter(OperationId, const ActivationKey &) override;
    Result<void> request_stop(const ActivationObservation &, TimePoint) override;
};
class ActivationClient {
  public:
    ActivationClient(HostActivationPort &, ActivationPolicyPort &, std::size_t max_requests = 128);
    Result<void> request(const ActivationRequest &, TimePoint now);
    Result<void> observe(const ActivationObservation &, TimePoint now);
    Result<void> detach(OperationId);
    Result<void> request_stop(OperationId, TimePoint deadline, TimePoint now);
    Result<ActivationObservation> inspect(OperationId);
    Result<void> authorize_model(OperationId);
    Result<void> authorize_stop(OperationId);
    void host_epoch_lost(HostEpoch) noexcept;
    void expire(TimePoint now) noexcept;
    std::size_t retire_detached() noexcept;

  private:
    struct Pending {
        ActivationRequest request;
        ActivationObservation current;
        bool detached{};
        bool stop_requested{};
    };
    HostActivationPort &gateway_;
    ActivationPolicyPort &policy_;
    std::size_t limit_;
    std::uint64_t operation_high_water_{};
    std::vector<Pending> pending_;
};

enum class AdmissionGateState { accepting, draining, closed };
struct IdleSnapshot {
    std::size_t nonterminal_jobs{};
    std::size_t duplicate_waiters{};
    std::size_t prepared_dispatches{};
    std::size_t model_loads{};
    std::size_t controller_maintenance{};
    std::size_t nontransferable_cleanup{};
    bool idle() const noexcept;
};
class AdmissionGate {
  public:
    explicit AdmissionGate(std::uint64_t initial_epoch = 1, std::size_t max_pending = 128);
    Result<std::uint64_t> claim();
    Result<void> settle_claim(std::uint64_t claim);
    Result<std::uint64_t> prepare_idle_stop(std::uint64_t expected_epoch, const IdleSnapshot &);
    Result<void> shutdown_confirmed(ContainmentProof);
    AdmissionGateState state() const noexcept { return state_; }
    std::uint64_t epoch() const noexcept { return epoch_; }
    std::size_t pending_claims() const noexcept { return claims_.size(); }

  private:
    AdmissionGateState state_{AdmissionGateState::accepting};
    std::uint64_t epoch_;
    std::uint64_t next_claim_{1};
    std::size_t limit_;
    std::vector<std::uint64_t> claims_;
};

enum class ForwardedAdmission { not_sent, rejected_before_run, admitted, unknown };
// A lost response never turns into a fresh unkeyed submit after process restart.
bool may_retry_submission(ForwardedAdmission, RuntimeInstanceId original, RuntimeInstanceId current,
                          bool explicit_key_retained) noexcept;

struct ModelResidencyKey {
    std::array<std::uint8_t, 32> configuration_fingerprint{};
    LogicalResourceId resource;
    HostEpoch host_epoch;
    NativeWorkerGeneration worker;
    bool operator==(const ModelResidencyKey &) const = default;
};
enum class ModelResidencyState { absent, loading, resident, releasing, unknown };
struct ModelResidencySnapshot {
    ModelResidencyKey key;
    ModelResidencyState state{ModelResidencyState::absent};
    std::optional<ResourceOwner> owner;
    std::optional<AllocationIdentity> allocation;
    bool pausable{};
};
enum class ModelLoadDecision { load_owned, wait_without_lease, resident_available };
class ModelLoadRegistry {
  public:
    explicit ModelLoadRegistry(ResourceManager &, std::size_t max_models = 128);
    ModelLoadDecision availability(const ModelResidencyKey &) const noexcept;
    Result<void> begin(const ModelResidencyKey &, ResourceOwner, const ExecutionLease &,
                       bool pausable, TimePoint deadline, TimePoint now);
    Result<void> completed(const ModelResidencyKey &, ResourceOwner, AllocationIdentity,
                           TimePoint now);
    Result<void> cancel(const ModelResidencyKey &, ResourceOwner);
    Result<void> cleanup_settled(const ModelResidencyKey &, ResourceOwner, ContainmentProof);
    Result<void> can_pause(const ModelResidencyKey &) const;
    Result<void> begin_release(const ModelResidencyKey &, AllocationIdentity);
    Result<void> released(const ModelResidencyKey &, AllocationIdentity);
    std::size_t retire_absent() noexcept;
    void fence(HostEpoch) noexcept;
    std::optional<ModelResidencySnapshot> inspect(const ModelResidencyKey &) const;

  private:
    struct ModelRecord {
        ModelResidencySnapshot snapshot;
        ExecutionLease lease;
        TimePoint deadline{};
        bool cancelled{};
    };
    ResourceManager &resources_;
    std::size_t limit_;
    std::vector<ModelRecord> models_;
};
} // namespace flamoris::runtime
