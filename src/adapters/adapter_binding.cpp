#include "flamoris/runtime/adapter_binding.hpp"
#include <algorithm>

namespace flamoris::runtime {
namespace {
std::uint64_t now_ms(MonotonicClock &clock) noexcept {
    return static_cast<std::uint64_t>(std::max<Duration::rep>(0, clock.now().count()) / 1000000);
}
AdapterOutcome closed() noexcept {
    AdapterOutcome outcome;
    outcome.error = ErrorEnvelope::make(ErrorCode::internal_error, ErrorStage::dispatch,
                                        ExternalOutcome::not_dispatched);
    return outcome;
}
} // namespace
Result<std::shared_ptr<PersistentAdapterBinding>>
PersistentAdapterBinding::create(CapabilityContract contract,
                                 std::shared_ptr<RegisteredProviderPort> provider,
                                 std::size_t max_operations, std::size_t max_queries) {
    if (!provider || !max_operations || max_operations > 65536 || max_queries > 1024)
        return Result<std::shared_ptr<PersistentAdapterBinding>>::failure(
            ErrorEnvelope::make(ErrorCode::invalid_request));
    try {
        return Result<std::shared_ptr<PersistentAdapterBinding>>::success(
            std::shared_ptr<PersistentAdapterBinding>(new PersistentAdapterBinding(
                std::move(contract), std::move(provider), max_operations, max_queries)));
    } catch (...) {
        return Result<std::shared_ptr<PersistentAdapterBinding>>::failure(
            ErrorEnvelope::make(ErrorCode::resource_unavailable));
    }
}
AdapterOutcome PersistentAdapterBinding::invoke(AdapterRequest request,
                                                MonotonicClock &clock) noexcept {
    try {
        std::lock_guard lock(mutex_);
        return adapter_.invoke(std::move(request), now_ms(clock));
    } catch (...) {
        return closed();
    }
}
AdapterOutcome PersistentAdapterBinding::reconcile(AdapterGrant grant,
                                                   MonotonicClock &clock) noexcept {
    try {
        std::lock_guard lock(mutex_);
        return adapter_.reconcile(std::move(grant), now_ms(clock));
    } catch (...) {
        return closed();
    }
}
bool PersistentAdapterBinding::try_retire_before(RuntimeInstanceId instance,
                                                 std::uint64_t minimum_live_run) noexcept {
    try {
        std::unique_lock lock(mutex_, std::try_to_lock);
        return lock.owns_lock() && adapter_.retire_before(instance, minimum_live_run);
    } catch (...) {
        return false;
    }
}
} // namespace flamoris::runtime
