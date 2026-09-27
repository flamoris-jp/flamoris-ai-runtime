#pragma once
#include "flamoris/runtime/registered_adapter.hpp"
#include <memory>
#include <mutex>

namespace flamoris::runtime {
// One persistent transcript per configured capability. Provider workers enter
// this gate; the control actor never waits for it or owns provider execution.
class PersistentAdapterBinding final {
  public:
    static Result<std::shared_ptr<PersistentAdapterBinding>>
    create(CapabilityContract, std::shared_ptr<RegisteredProviderPort>,
           std::size_t max_operations = 65536, std::size_t max_reconciliation_queries = 4);
    AdapterOutcome invoke(AdapterRequest, MonotonicClock &) noexcept;
    AdapterOutcome reconcile(AdapterGrant, MonotonicClock &) noexcept;
    bool try_retire_before(RuntimeInstanceId, std::uint64_t minimum_live_run) noexcept;

  private:
    PersistentAdapterBinding(CapabilityContract contract,
                             std::shared_ptr<RegisteredProviderPort> provider,
                             std::size_t max_operations, std::size_t max_queries)
        : provider_(std::move(provider)),
          adapter_(std::move(contract), *provider_, max_operations, max_queries) {}
    std::shared_ptr<RegisteredProviderPort> provider_;
    RegisteredCapabilityAdapter adapter_;
    std::mutex mutex_;
};
} // namespace flamoris::runtime
