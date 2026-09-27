#pragma once
#include "flamoris/runtime/facade.hpp"
#include "flamoris/runtime/inference.hpp"
#include "flamoris/runtime/registered_adapter.hpp"
#include "flamoris/runtime/resources.hpp"
#include "flamoris/runtime/submission.hpp"
#include <functional>
#include <memory>

namespace flamoris::runtime {
struct RuntimeRegistration {
    std::string capability;
    LogicalResourceId resource{1};
    NativeWorkerGeneration worker_generation{1};
    ResourceVector requirements;
    ResourceVector run_resource_limit;
    std::shared_ptr<RegisteredProviderPort> provider;
    std::optional<NativeWorkerConfig> native;
    std::string prompt_field{"prompt"};
};
using RuntimeHostRelease = HostReleaseReceipt;
struct RuntimeConfiguration {
    RuntimeInstanceId instance;
    std::shared_ptr<MonotonicClock> clock;
    std::shared_ptr<HostAuthorityPort> host;
    CapabilitySnapshot capabilities;
    PolicySnapshot policy;
    CompilerProfile compiler;
    SubmissionBounds submissions;
    // Trusted composition/test seams; invoked outside the control actor.
    std::function<Result<std::unique_ptr<NativeWorkerPort>>(NativeWorkerConfig)>
        native_worker_factory;
    std::function<void(PendingSubmissionId, bool owner)> submission_preparation_hook;
    std::vector<RuntimeRegistration> registrations;
    std::size_t max_runs{32}, normal_commands{128}, mandatory_commands{128}, max_workers{16};
};
// One actor owns lifecycle, scheduling, policy, submission and resource decisions.
// Provider/native work occurs outside that actor; transport methods exchange owned values.
class RuntimeInstance final : public RuntimeCommandPort {
  public:
    static Result<std::unique_ptr<RuntimeInstance>> create(RuntimeConfiguration);
    ~RuntimeInstance() override;
    RuntimeInstance(const RuntimeInstance &) = delete;
    RuntimeInstance &operator=(const RuntimeInstance &) = delete;
    RuntimeInstanceId instance_id() const noexcept;
    Result<SubmissionReceipt> submit(const AuthorizationContext &, std::string_view) override;
    Result<RunSnapshot> status(const AuthorizationContext &, RunId) override;
    Result<RunResult> result(const AuthorizationContext &, RunId) override;
    Result<CommandReceipt> cancel(const AuthorizationContext &, RunId) override;
    Result<CommandReceipt> pause(const AuthorizationContext &, RunId, std::uint64_t) override;
    Result<CommandReceipt> resume(const AuthorizationContext &, RunId, std::uint64_t) override;
    Result<ObservationPage> events(const AuthorizationContext &, RunId, std::uint64_t,
                                   std::size_t) override;
    Result<ReplaySnapshot> replay(const AuthorizationContext &, RunId) override;
    // Trusted host/administrative binding only. These are intentionally absent from
    // portable Workflow JSON and CallerFacade. Envelopes/receipts are still epoch checked.
    Result<void> observe_host_envelope(HostEnvelope);
    Result<void> observe_host_acquisition(HostAcquisition);
    Result<void> observe_host_release(RuntimeHostRelease);
    Result<void> replace_policy(PolicySnapshot);
    Result<void> replace_capabilities(CapabilitySnapshot);
    Result<ResourceSnapshot> resource_snapshot(LogicalResourceId);
    // A synchronization barrier, useful with an injected manual clock; never sleeps.
    Result<void> poll();
    Result<CommandReceipt> propose_children(const AuthorizationContext &, RunId, JobId,
                                            std::string_view);
    // Closes admission and requests cancellation. Native destruction/join is outside
    // the control thread; absent host release keeps lifecycle finalizing/accounted.
    Result<void> shutdown();
    Result<std::uint64_t> admission_epoch();
    Result<std::uint64_t> prepare_idle_stop(std::uint64_t expected_epoch);
    Result<void> fence_admission();

  private:
    struct Impl;
    explicit RuntimeInstance(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
} // namespace flamoris::runtime
