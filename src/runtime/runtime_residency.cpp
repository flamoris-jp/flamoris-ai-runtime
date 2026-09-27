#include "flamoris/runtime/runtime_residency.hpp"
#include <algorithm>

namespace flamoris::runtime {
namespace {
template <class T> Result<T> failure(ErrorCode code) {
    return Result<T>::failure(ErrorEnvelope::make(code, ErrorStage::execution));
}
} // namespace
RuntimeResidencyPool::RuntimeResidencyPool(ResourceManager &resources, std::size_t models,
                                           std::size_t jobs)
    : resources_(resources), registry_(resources, models), model_limit_(models), job_limit_(jobs) {
    records_.reserve(models);
}
RuntimeResidencyPool::Record *RuntimeResidencyPool::find(const ModelResidencyKey &key) noexcept {
    auto it = std::find_if(records_.begin(), records_.end(),
                           [&](const auto &record) { return record.key == key; });
    return it == records_.end() ? nullptr : &*it;
}
const RuntimeResidencyPool::Record *
RuntimeResidencyPool::find(const ModelResidencyKey &key) const noexcept {
    auto it = std::find_if(records_.begin(), records_.end(),
                           [&](const auto &record) { return record.key == key; });
    return it == records_.end() ? nullptr : &*it;
}
ModelLoadDecision RuntimeResidencyPool::availability(const ModelResidencyKey &key,
                                                     TimePoint now) const noexcept {
    auto decision = registry_.availability(key);
    if (decision != ModelLoadDecision::resident_available)
        return decision;
    auto *record = find(key);
    if (!record || !record->loaded || !record->holder || !record->allocation || record->eviction ||
        !resources_.compatible(*record->allocation, key.resource, key.host_epoch, key.worker, now))
        return ModelLoadDecision::wait_without_lease;
    return decision;
}
Result<void> RuntimeResidencyPool::begin(const ModelResidencyKey &key, ResourceOwner owner,
                                         const ExecutionLease &lease, bool pausable,
                                         TimePoint deadline, TimePoint now) {
    if (job_limit_ == 0 || availability(key, now) != ModelLoadDecision::load_owned)
        return failure<void>(ErrorCode::resource_unavailable);
    auto *record = find(key);
    if (record && !record->released)
        return failure<void>(ErrorCode::state_unavailable);
    if (!record && records_.size() >= model_limit_)
        return failure<void>(ErrorCode::resource_unavailable);
    Record prepared{key, owner, std::nullopt, {}, {}, {}, std::nullopt, false, false};
    prepared.jobs.reserve(job_limit_);
    prepared.jobs.push_back(owner.job);
    auto begun = registry_.begin(key, owner, lease, pausable, deadline, now);
    if (!begun)
        return begun;
    if (record)
        *record = std::move(prepared);
    else
        records_.push_back(std::move(prepared));
    return Result<void>::success();
}
Result<void> RuntimeResidencyPool::completed(const ModelResidencyKey &key, ResourceOwner owner,
                                             std::shared_ptr<const TinyModel> model,
                                             AllocationIdentity allocation, TimePoint now) {
    auto *record = find(key);
    if (!record || record->owner != owner || !model || record->eviction || record->released)
        return failure<void>(ErrorCode::state_unavailable);
    auto ledger = resources_.allocation(allocation);
    if (!ledger || ledger->resource != key.resource ||
        ledger->footprint[ResourceKind::ram] < model->resident_bytes())
        return failure<void>(ErrorCode::resource_unavailable);
    if (record->loaded)
        return record->holder == model && record->allocation == allocation
                   ? Result<void>::success()
                   : failure<void>(ErrorCode::invalid_request);
    auto complete = registry_.completed(key, owner, allocation, now);
    if (!complete)
        return complete;
    record->allocation = allocation;
    record->holder = std::move(model);
    record->witness = record->holder;
    record->loaded = true;
    return Result<void>::success();
}
Result<ResidentModelView> RuntimeResidencyPool::view(const ModelResidencyKey &key,
                                                     TimePoint now) const {
    auto *record = find(key);
    if (!record || availability(key, now) != ModelLoadDecision::resident_available)
        return failure<ResidentModelView>(ErrorCode::state_unavailable);
    return Result<ResidentModelView>::success(
        {*record->allocation, record->holder->resident_bytes()});
}
Result<std::shared_ptr<const TinyModel>>
RuntimeResidencyPool::attach(const ModelResidencyKey &key, JobId job,
                             const ResourceVector &run_limit, TimePoint now) {
    auto *record = find(key);
    if (!record || availability(key, now) != ModelLoadDecision::resident_available)
        return failure<std::shared_ptr<const TinyModel>>(ErrorCode::state_unavailable);
    if (std::find(record->jobs.begin(), record->jobs.end(), job) != record->jobs.end())
        return Result<std::shared_ptr<const TinyModel>>::success(record->holder);
    if (record->jobs.size() >= job_limit_)
        return failure<std::shared_ptr<const TinyModel>>(ErrorCode::resource_unavailable);
    auto added = resources_.add_reference(*record->allocation, job, run_limit);
    if (!added)
        return Result<std::shared_ptr<const TinyModel>>::failure(added.error());
    record->jobs.push_back(job);
    return Result<std::shared_ptr<const TinyModel>>::success(record->holder);
}
Result<bool> RuntimeResidencyPool::release_job(const ModelResidencyKey &key, JobId job,
                                               OperationId eviction, TimePoint now) {
    auto *record = find(key);
    if (!record || !record->loaded || !record->allocation || !eviction.valid())
        return failure<bool>(ErrorCode::state_unavailable);
    if (record->released)
        return Result<bool>::success(true);
    auto user = std::find(record->jobs.begin(), record->jobs.end(), job);
    if (user == record->jobs.end())
        return record->eviction ? reconcile(key, now) : Result<bool>::success(false);
    auto dropped = resources_.drop_reference(*record->allocation, job);
    if (!dropped)
        return Result<bool>::failure(dropped.error());
    record->jobs.erase(user);
    if (!record->jobs.empty())
        return Result<bool>::success(false);
    auto releasing = registry_.begin_release(key, *record->allocation);
    if (!releasing)
        return Result<bool>::failure(releasing.error());
    record->eviction = eviction;
    record->holder.reset();
    return reconcile(key, now);
}
Result<bool> RuntimeResidencyPool::reconcile(const ModelResidencyKey &key, TimePoint now) {
    auto *record = find(key);
    if (!record || !record->allocation || !record->eviction)
        return failure<bool>(ErrorCode::state_unavailable);
    if (record->released)
        return Result<bool>::success(true);
    if (!record->jobs.empty() || !record->witness.expired())
        return Result<bool>::success(false);
    auto requested = resources_.request_release(*record->allocation, *record->eviction);
    if (!requested)
        return Result<bool>::failure(requested.error());
    auto released =
        resources_.release_confirmed({*record->allocation, *record->eviction, true}, now);
    if (!released)
        return Result<bool>::failure(released.error());
    auto settled = registry_.released(key, *record->allocation);
    if (!settled)
        return Result<bool>::failure(settled.error());
    record->released = true;
    return Result<bool>::success(true);
}
Result<void> RuntimeResidencyPool::cancel_loading(const ModelResidencyKey &key,
                                                  ResourceOwner owner) {
    return registry_.cancel(key, owner);
}
Result<void> RuntimeResidencyPool::loading_stopped(const ModelResidencyKey &key,
                                                   ResourceOwner owner, ContainmentProof proof) {
    auto result = registry_.cleanup_settled(key, owner, proof);
    if (result) {
        auto *record = find(key);
        if (record && !record->loaded) {
            record->jobs.clear();
            record->released = true;
        }
    }
    return result;
}
Result<void> RuntimeResidencyPool::can_pause(const ModelResidencyKey &key) const {
    return registry_.can_pause(key);
}
void RuntimeResidencyPool::fence(HostEpoch epoch) noexcept { registry_.fence(epoch); }
std::size_t RuntimeResidencyPool::retire_settled() noexcept {
    registry_.retire_absent();
    const auto old = records_.size();
    std::erase_if(records_, [](const auto &record) {
        return record.released && record.jobs.empty() && record.witness.expired();
    });
    return old - records_.size();
}
std::size_t RuntimeResidencyPool::loading_count() const noexcept {
    return static_cast<std::size_t>(
        std::count_if(records_.begin(), records_.end(),
                      [](const auto &record) { return !record.loaded && !record.released; }));
}
} // namespace flamoris::runtime
