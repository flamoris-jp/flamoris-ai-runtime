#pragma once
#include "flamoris/runtime/activation.hpp"
#include <algorithm>
#include <stdexcept>
#include <vector>

namespace flamoris::runtime::test {
class ManualActivationPolicy final : public ActivationPolicyPort {
  public:
    std::array<bool, 4> allowed{true, true, true, true};
    std::array<unsigned, 4> checks{};
    bool throw_on_check{};
    Result<void> authorize(ActivationScope scope, const ActivationKey &) override {
        ++checks[static_cast<std::size_t>(scope)];
        if (throw_on_check)
            throw std::runtime_error("private policy diagnostics");
        return allowed[static_cast<std::size_t>(scope)]
                   ? Result<void>::success()
                   : Result<void>::failure(ErrorEnvelope::make(ErrorCode::permission_denied,
                                                               ErrorStage::admission,
                                                               ExternalOutcome::not_dispatched));
    }
};
// This is an external-authority test double, never a Kernel service manager.
// Starts and resource obligations are deliberately independent of the client.
class ManualActivationGateway final : public HostActivationPort {
  public:
    struct Record {
        ActivationKey key;
        ActivationGeneration generation;
        HostEpoch epoch;
        ProcessStatus status{ProcessStatus::starting};
        TimePoint deadline{};
        std::vector<OperationId> waiters;
        RuntimeInstanceId instance;
        bool start_sent{};
        bool uncertain_resources{};
    };
    explicit ManualActivationGateway(ManualActivationPolicy &policy) : policy_(policy) {
        records.reserve(8);
        observations.reserve(128);
    }
    HostEpoch epoch{1};
    std::size_t host_starts{}, host_stops{};
    bool throw_after_request{};
    std::vector<Record> records;
    std::vector<ActivationObservation> observations;
    Result<void> request(const ActivationRequest &request) override {
        auto permission = policy_.authorize(ActivationScope::activate, request.key);
        if (!permission)
            return permission;
        if (request.expected_epoch != epoch)
            return fail(ErrorCode::native_compute_unavailable);
        auto it = std::find_if(records.begin(), records.end(), [&](const auto &r) {
            return r.key.service == request.key.service &&
                   r.key.ownership_scope == request.key.ownership_scope;
        });
        if (it != records.end() && it->status != ProcessStatus::stopped) {
            if (it->key != request.key)
                return fail(ErrorCode::invalid_request);
            if (it->status != ProcessStatus::starting && it->status != ProcessStatus::ready)
                return fail(ErrorCode::native_compute_unavailable);
            if (it->waiters.size() >= 4)
                return fail(ErrorCode::resource_unavailable);
            it->waiters.push_back(request.request);
            emit(*it, request.request);
            return Result<void>::success();
        }
        if (records.size() >= 8 && it == records.end())
            return fail(ErrorCode::resource_unavailable);
        Record r{request.key,
                 ActivationGeneration{next_generation_++},
                 epoch,
                 ProcessStatus::starting,
                 request.deadline,
                 {request.request},
                 {},
                 false,
                 false};
        if (it == records.end()) {
            records.push_back(r);
            it = records.end() - 1;
        } else
            *it = r;
        emit(*it, request.request);
        if (throw_after_request)
            throw std::runtime_error("private host diagnostics");
        return Result<void>::success();
    }
    Result<void> detach_waiter(OperationId id, const ActivationKey &key) override {
        for (auto &r : records)
            if (r.key == key) {
                auto it = std::find(r.waiters.begin(), r.waiters.end(), id);
                if (it != r.waiters.end())
                    r.waiters.erase(it);
            }
        return Result<void>::success();
    }
    Result<void> request_stop(const ActivationObservation &observation, TimePoint) override {
        auto permission = policy_.authorize(ActivationScope::stop, observation.key);
        if (!permission)
            return permission;
        auto it = std::find_if(records.begin(), records.end(), [&](const auto &r) {
            return r.key == observation.key && r.generation == observation.generation &&
                   r.epoch == observation.host_epoch;
        });
        if (it == records.end())
            return fail(ErrorCode::state_unavailable);
        ++host_stops;
        it->status =
            it->status == ProcessStatus::ready ? ProcessStatus::draining : ProcessStatus::stopping;
        for (auto id : it->waiters)
            emit(*it, id);
        return Result<void>::success();
    }
    Result<void> perform_start(std::size_t index, TimePoint now) {
        auto &r = records.at(index);
        auto permission = policy_.authorize(ActivationScope::activate, r.key);
        if (!permission) {
            r.status = ProcessStatus::failed;
            return permission;
        }
        if (r.start_sent || r.status != ProcessStatus::starting || now >= r.deadline)
            return fail(ErrorCode::native_compute_unavailable);
        ++host_starts;
        r.start_sent = true;
        return Result<void>::success();
    }
    bool ready(std::size_t index, RuntimeInstanceId instance, TimePoint now = {}) {
        auto &r = records.at(index);
        if (!r.start_sent || r.status != ProcessStatus::starting || r.epoch != epoch ||
            now >= r.deadline)
            return false;
        r.status = ProcessStatus::ready;
        r.instance = instance;
        for (auto id : r.waiters)
            emit(r, id);
        return true;
    }
    void expire(TimePoint now) {
        for (auto &record : records)
            if (record.status == ProcessStatus::starting && now >= record.deadline) {
                record.status = record.start_sent ? ProcessStatus::unknown : ProcessStatus::stopped;
                record.uncertain_resources = record.start_sent;
                for (auto id : record.waiters)
                    emit(record, id);
            }
    }
    void partial_failure(std::size_t index) {
        auto &r = records.at(index);
        r.status = ProcessStatus::unknown;
        r.uncertain_resources = true;
        for (auto id : r.waiters)
            emit(r, id);
    }
    void stop_confirmed(std::size_t index) {
        auto &r = records.at(index);
        r.status = ProcessStatus::stopped;
        r.uncertain_resources = false;
        for (auto id : r.waiters)
            emit(r, id);
    }
    void lose_epoch() {
        epoch = HostEpoch{epoch.value() + 1};
        for (auto &r : records) {
            r.status = ProcessStatus::unknown;
            r.uncertain_resources = true;
        }
    }

  private:
    ManualActivationPolicy &policy_;
    std::uint64_t next_generation_{1};
    static Result<void> fail(ErrorCode code) {
        return Result<void>::failure(
            ErrorEnvelope::make(code, ErrorStage::admission, ExternalOutcome::not_dispatched));
    }
    void emit(const Record &r, OperationId id) {
        observations.push_back({id, r.key, r.generation, r.epoch, r.status, r.instance,
                                r.status == ProcessStatus::ready ? 1U : 0U,
                                r.status == ProcessStatus::stopped});
    }
};
} // namespace flamoris::runtime::test
