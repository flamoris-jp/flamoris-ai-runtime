#pragma once
#include "flamoris/runtime/activation.hpp"
#include "flamoris/runtime/native.hpp"
#include <memory>

namespace flamoris::runtime {
struct ResidentModelView {
    AllocationIdentity allocation;
    std::size_t model_bytes{};
};
// Runtime-owned immutable residency; the existing Job remains loader and each
// session owns its mutable state. This pool never loads or executes a model.
class RuntimeResidencyPool {
  public:
    RuntimeResidencyPool(ResourceManager &, std::size_t max_models = 128,
                         std::size_t max_jobs_per_model = 256);
    ModelLoadDecision availability(const ModelResidencyKey &, TimePoint now) const noexcept;
    Result<void> begin(const ModelResidencyKey &, ResourceOwner, const ExecutionLease &,
                       bool pausable, TimePoint deadline, TimePoint now);
    // Call only after accounting the physical allocation and complete manifest.
    // Move the load observation holder here; do not retain another receipt copy.
    Result<void> completed(const ModelResidencyKey &, ResourceOwner,
                           std::shared_ptr<const TinyModel>, AllocationIdentity, TimePoint now);
    Result<ResidentModelView> view(const ModelResidencyKey &, TimePoint now) const;
    // The caller has passed current policy and committed a lease containing the
    // shared allocation. It transfers this immutable holder into the new worker.
    Result<std::shared_ptr<const TinyModel>> attach(const ModelResidencyKey &, JobId,
                                                    const ResourceVector &run_limit, TimePoint now);
    // A real session-release receipt must precede this call. Logical last-use
    // alone does not prove physical release; an outstanding holder remains debt.
    Result<bool> release_job(const ModelResidencyKey &, JobId, OperationId eviction, TimePoint now);
    Result<bool> reconcile(const ModelResidencyKey &, TimePoint now);
    Result<void> cancel_loading(const ModelResidencyKey &, ResourceOwner);
    Result<void> loading_stopped(const ModelResidencyKey &, ResourceOwner, ContainmentProof);
    Result<void> can_pause(const ModelResidencyKey &) const;
    void fence(HostEpoch) noexcept;
    std::size_t retire_settled() noexcept;
    std::size_t loading_count() const noexcept;

  private:
    struct Record {
        ModelResidencyKey key;
        ResourceOwner owner;
        std::optional<AllocationIdentity> allocation;
        std::shared_ptr<const TinyModel> holder;
        std::weak_ptr<const TinyModel> witness;
        std::vector<JobId> jobs;
        std::optional<OperationId> eviction;
        bool loaded{};
        bool released{};
    };
    Record *find(const ModelResidencyKey &) noexcept;
    const Record *find(const ModelResidencyKey &) const noexcept;
    ResourceManager &resources_;
    ModelLoadRegistry registry_;
    std::size_t model_limit_, job_limit_;
    std::vector<Record> records_;
};
} // namespace flamoris::runtime
