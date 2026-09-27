#include "flamoris/runtime/runtime_activation.hpp"
#include <algorithm>

namespace flamoris::runtime {
namespace {
template <class T> Result<T> failure(ErrorCode code = ErrorCode::native_compute_unavailable) {
    return Result<T>::failure(ErrorEnvelope::make(code, ErrorStage::admission));
}
} // namespace
RuntimeActivationClient::RuntimeActivationClient(RuntimeActivationConfiguration config)
    : config_(std::move(config)), client_(*config_.gateway, *config_.policy, config_.max_waiters) {
    bindings_.reserve(config_.max_waiters);
}
Result<std::unique_ptr<RuntimeActivationClient>>
RuntimeActivationClient::create(RuntimeActivationConfiguration config) {
    if (!config.gateway || !config.policy || !config.host_epoch.valid() ||
        config.key.service.empty() || config.key.service.size() > 64 || !config.max_waiters ||
        config.max_waiters > 4096)
        return failure<std::unique_ptr<RuntimeActivationClient>>(ErrorCode::invalid_request);
    try {
        if (!config.clock)
            config.clock = std::make_shared<SteadyMonotonicClock>();
        return Result<std::unique_ptr<RuntimeActivationClient>>::success(
            std::unique_ptr<RuntimeActivationClient>(
                new RuntimeActivationClient(std::move(config))));
    } catch (...) {
        return failure<std::unique_ptr<RuntimeActivationClient>>(ErrorCode::resource_unavailable);
    }
}
Result<void> RuntimeActivationClient::trigger(OperationId waiter, TimePoint deadline) {
    std::lock_guard lock(mutex_);
    return client_.request({waiter, config_.key, config_.host_epoch, deadline},
                           config_.clock->now());
}
Result<void> RuntimeActivationClient::observe(ActivationObservation observation) {
    std::shared_ptr<RuntimeInstance> fence;
    {
        std::lock_guard lock(mutex_);
        auto accepted = client_.observe(observation, config_.clock->now());
        if (!accepted)
            return accepted;
        if (observation.status != ProcessStatus::ready) {
            auto it = std::find_if(bindings_.begin(), bindings_.end(),
                                   [&](const auto &b) { return b.waiter == observation.request; });
            if (it != bindings_.end()) {
                fence = it->endpoint.lock();
                bindings_.erase(it);
            }
        }
    }
    return fence ? fence->fence_admission() : Result<void>::success();
}
Result<void>
RuntimeActivationClient::attach_endpoint(OperationId waiter,
                                         const std::shared_ptr<RuntimeInstance> &endpoint) {
    if (!endpoint)
        return failure<void>(ErrorCode::invalid_request);
    ActivationObservation ready;
    {
        std::lock_guard lock(mutex_);
        auto observed = client_.inspect(waiter);
        if (!observed)
            return Result<void>::failure(observed.error());
        ready = observed.value();
        if (ready.status != ProcessStatus::ready || ready.instance != endpoint->instance_id())
            return failure<void>();
    }
    auto epoch = endpoint->admission_epoch();
    if (!epoch || epoch.value() != ready.admission_epoch)
        return failure<void>();
    std::lock_guard lock(mutex_);
    client_.expire(config_.clock->now());
    auto current = client_.inspect(waiter);
    if (!current || current.value() != ready)
        return failure<void>();
    auto found = std::find_if(bindings_.begin(), bindings_.end(),
                              [&](const auto &b) { return b.waiter == waiter; });
    if (found != bindings_.end()) {
        *found = {waiter, ready, endpoint};
        return Result<void>::success();
    }
    if (bindings_.size() >= config_.max_waiters)
        return failure<void>(ErrorCode::resource_unavailable);
    bindings_.push_back({waiter, ready, endpoint});
    return Result<void>::success();
}
Result<void> RuntimeActivationClient::detach(OperationId waiter) {
    std::lock_guard lock(mutex_);
    auto detached = client_.detach(waiter);
    if (!detached)
        return detached;
    std::erase_if(bindings_, [&](const auto &b) { return b.waiter == waiter; });
    return Result<void>::success();
}
Result<ActivationObservation> RuntimeActivationClient::status(OperationId waiter) {
    std::lock_guard lock(mutex_);
    client_.expire(config_.clock->now());
    return client_.inspect(waiter);
}
Result<void> RuntimeActivationClient::authorize_model(OperationId waiter) {
    std::lock_guard lock(mutex_);
    return client_.authorize_model(waiter);
}
Result<RuntimeActivationClient::ReadyEndpoint>
RuntimeActivationClient::ready_endpoint(OperationId waiter) {
    std::lock_guard lock(mutex_);
    client_.expire(config_.clock->now());
    auto current = client_.inspect(waiter);
    if (!current)
        return Result<ReadyEndpoint>::failure(current.error());
    if (current.value().status != ProcessStatus::ready)
        return failure<ReadyEndpoint>();
    auto found = std::find_if(bindings_.begin(), bindings_.end(),
                              [&](const auto &b) { return b.waiter == waiter; });
    if (found == bindings_.end() || found->ready != current.value())
        return failure<ReadyEndpoint>();
    auto endpoint = found->endpoint.lock();
    if (!endpoint)
        return failure<ReadyEndpoint>();
    return Result<ReadyEndpoint>::success({std::move(endpoint), found->ready.admission_epoch});
}
Result<SubmissionReceipt> RuntimeActivationClient::submit(OperationId waiter,
                                                          const AuthorizationContext &context,
                                                          std::string_view request) {
    auto ready = ready_endpoint(waiter);
    if (!ready)
        return Result<SubmissionReceipt>::failure(ready.error());
    auto epoch = ready.value().endpoint->admission_epoch();
    if (!epoch || epoch.value() != ready.value().admission_epoch)
        return failure<SubmissionReceipt>();
    return ready.value().endpoint->submit(context, request);
}
Result<void> RuntimeActivationClient::request_idle_stop(OperationId waiter, TimePoint deadline) {
    {
        std::lock_guard lock(mutex_);
        if (deadline <= config_.clock->now())
            return failure<void>(ErrorCode::invalid_request);
        auto permission = client_.authorize_stop(waiter);
        if (!permission)
            return permission;
    }
    auto ready = ready_endpoint(waiter);
    if (!ready)
        return Result<void>::failure(ready.error());
    auto closed = ready.value().endpoint->prepare_idle_stop(ready.value().admission_epoch);
    if (!closed)
        return Result<void>::failure(closed.error());
    std::lock_guard lock(mutex_);
    std::erase_if(bindings_, [&](const auto &b) { return b.waiter == waiter; });
    return client_.request_stop(waiter, deadline, config_.clock->now());
}
Result<void> RuntimeActivationClient::host_epoch_lost(HostEpoch epoch) {
    std::vector<std::shared_ptr<RuntimeInstance>> endpoints;
    {
        std::lock_guard lock(mutex_);
        client_.host_epoch_lost(epoch);
        for (const auto &b : bindings_)
            if (b.ready.host_epoch == epoch)
                if (auto endpoint = b.endpoint.lock()) {
                    if (std::find(endpoints.begin(), endpoints.end(), endpoint) == endpoints.end())
                        endpoints.push_back(std::move(endpoint));
                }
        std::erase_if(bindings_, [&](const auto &b) { return b.ready.host_epoch == epoch; });
    }
    for (auto &endpoint : endpoints) {
        auto fenced = endpoint->fence_admission();
        if (!fenced)
            return fenced;
    }
    return Result<void>::success();
}
std::size_t RuntimeActivationClient::retire_detached() {
    std::lock_guard lock(mutex_);
    std::erase_if(bindings_, [](const auto &b) { return b.endpoint.expired(); });
    return client_.retire_detached();
}
} // namespace flamoris::runtime
