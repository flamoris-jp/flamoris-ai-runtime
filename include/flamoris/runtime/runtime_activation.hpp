#pragma once
#include "flamoris/runtime/activation.hpp"
#include "flamoris/runtime/runtime.hpp"
#include <memory>
#include <mutex>

namespace flamoris::runtime {
struct RuntimeActivationConfiguration {
    ActivationKey key;
    HostEpoch host_epoch;
    std::shared_ptr<MonotonicClock> clock;
    std::shared_ptr<HostActivationPort> gateway;
    std::shared_ptr<ActivationPolicyPort> policy;
    std::size_t max_waiters{128};
};
// Caller-side configured gateway client. The running Kernel never launches itself.
// Endpoint references are weak; late host callbacks cannot prolong or dereference
// a destroyed Runtime. Startup does not bypass its admission/resource checks.
class RuntimeActivationClient {
  public:
    static Result<std::unique_ptr<RuntimeActivationClient>> create(RuntimeActivationConfiguration);
    Result<void> trigger(OperationId waiter, TimePoint startup_deadline);
    Result<void> observe(ActivationObservation);
    Result<void> attach_endpoint(OperationId waiter, const std::shared_ptr<RuntimeInstance> &);
    Result<void> detach(OperationId waiter);
    Result<ActivationObservation> status(OperationId waiter);
    Result<void> authorize_model(OperationId waiter);
    Result<SubmissionReceipt> submit(OperationId waiter, const AuthorizationContext &,
                                     std::string_view);
    Result<void> request_idle_stop(OperationId waiter, TimePoint stop_deadline);
    Result<void> host_epoch_lost(HostEpoch);
    std::size_t retire_detached();

  private:
    explicit RuntimeActivationClient(RuntimeActivationConfiguration);
    struct Binding {
        OperationId waiter;
        ActivationObservation ready;
        std::weak_ptr<RuntimeInstance> endpoint;
    };
    struct ReadyEndpoint {
        std::shared_ptr<RuntimeInstance> endpoint;
        std::uint64_t admission_epoch{};
    };
    Result<ReadyEndpoint> ready_endpoint(OperationId);
    RuntimeActivationConfiguration config_;
    ActivationClient client_;
    std::mutex mutex_;
    std::vector<Binding> bindings_;
};
} // namespace flamoris::runtime
