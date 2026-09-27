#include "flamoris/runtime/runtime_supervisor.hpp"
#include <atomic>
#include <catch_amalgamated.hpp>
#include <latch>
#include <thread>
#include <vector>
using namespace flamoris::runtime;

namespace {
struct WorkerState {
    std::latch release{1};
    std::latch entered{1};
    std::atomic<bool> finished{}, acknowledged{}, destroyed{};
    std::atomic<unsigned> joins{};
};
class BlockedOwner final : public RuntimeRetainedOwner, public RuntimeCleanupEndpoint {
  public:
    explicit BlockedOwner(std::shared_ptr<WorkerState> state)
        : state_(std::move(state)), worker_([state = state_] {
              state->entered.count_down();
              state->release.wait();
              state->finished = true;
          }) {}
    ~BlockedOwner() override { state_->destroyed = true; }
    bool cleanup_complete() const noexcept override {
        return state_->finished && state_->acknowledged;
    }
    void close_and_join() noexcept override {
        ++state_->joins;
        worker_.join();
    }
    Result<void> observe_host_envelope(HostEnvelope) override {
        return Result<void>::failure(ErrorEnvelope::make(ErrorCode::state_unavailable));
    }
    Result<void> observe_host_acquisition(HostAcquisition) override {
        return Result<void>::failure(ErrorEnvelope::make(ErrorCode::state_unavailable));
    }
    Result<void> observe_host_release(HostReleaseReceipt receipt) override {
        if (receipt.operation != OperationId{71})
            return Result<void>::failure(ErrorEnvelope::make(ErrorCode::state_unavailable));
        state_->acknowledged = true;
        return Result<void>::success();
    }

  private:
    std::shared_ptr<WorkerState> state_;
    std::jthread worker_;
};
// Prevent a failed assertion from leaving the test's deliberately blocked worker.
struct UnblockOnExit {
    std::shared_ptr<WorkerState> state;
    ~UnblockOnExit() {
        if (state)
            state->release.count_down();
    }
    void release() {
        state->release.count_down();
        state.reset();
    }
};
} // namespace

TEST_CASE("B-DRAIN supervisor retains the complete blocked owner and weak cleanup endpoint") {
    auto &supervisor = RuntimeSupervisor::process();
    REQUIRE(supervisor.collect() == 0);
    const auto baseline = supervisor.snapshot();
    auto slot = supervisor.reserve();
    REQUIRE(slot);
    auto state = std::make_shared<WorkerState>();
    auto owner = std::make_shared<BlockedOwner>(state);
    UnblockOnExit unblock{state};
    state->entered.wait();
    std::weak_ptr<RuntimeCleanupEndpoint> endpoint = owner;
    REQUIRE(slot.value().retain(owner));
    REQUIRE_FALSE(slot.value().valid());
    owner.reset();
    REQUIRE(supervisor.snapshot().retained == baseline.retained + 1);
    REQUIRE(supervisor.collect() == 0);
    REQUIRE_FALSE(state->destroyed);
    REQUIRE(state->joins == 0);
    auto callback = endpoint.lock();
    REQUIRE(callback);
    HostReleaseReceipt receipt;
    receipt.operation = OperationId{70};
    REQUIRE_FALSE(callback->observe_host_release(receipt));
    receipt.operation = OperationId{71};
    REQUIRE(callback->observe_host_release(receipt));
    callback.reset();
    REQUIRE(supervisor.collect() == 0); // An acknowledged host release does not stop a writer.
    REQUIRE_FALSE(state->destroyed);
    unblock.release();
    while (!state->finished)
        std::this_thread::yield();
    REQUIRE(supervisor.collect() == 1);
    REQUIRE(state->joins == 1);
    REQUIRE(state->destroyed);
    REQUIRE(endpoint.expired());
    REQUIRE(supervisor.snapshot().retained == baseline.retained);
    REQUIRE(supervisor.collect() == 0);
}

TEST_CASE("B-DRAIN reservation capacity fails before work and slots can be reused") {
    auto &supervisor = RuntimeSupervisor::process();
    (void)supervisor.collect();
    const auto baseline = supervisor.snapshot();
    REQUIRE(baseline.reserved == 0);
    REQUIRE(baseline.retained == 0);
    std::vector<RuntimeRetentionSlot> reservations;
    reservations.reserve(RuntimeSupervisor::capacity);
    for (std::size_t i = 0; i < RuntimeSupervisor::capacity; ++i) {
        auto slot = supervisor.reserve();
        REQUIRE(slot);
        reservations.push_back(std::move(slot).value());
    }
    auto rejected = supervisor.reserve();
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code() == ErrorCode::resource_unavailable);
    REQUIRE(rejected.error().reason() == ErrorReason::limit_exceeded);
    REQUIRE(supervisor.snapshot().reserved == RuntimeSupervisor::capacity);
    reservations.pop_back();
    auto replacement = supervisor.reserve();
    REQUIRE(replacement);
    auto moved = std::move(replacement).value();
    REQUIRE_FALSE(replacement.value().valid());
    REQUIRE(moved.valid());
    reservations.clear();
    REQUIRE(supervisor.snapshot().reserved == 1);
    moved = RuntimeRetentionSlot{};
    REQUIRE(supervisor.snapshot().reserved == 0);
}

TEST_CASE("B-DRAIN concurrent reapers close a retained owner exactly once") {
    auto &supervisor = RuntimeSupervisor::process();
    auto slot = supervisor.reserve();
    auto duplicate = supervisor.reserve();
    REQUIRE(slot);
    REQUIRE(duplicate);
    auto state = std::make_shared<WorkerState>();
    auto owner = std::make_shared<BlockedOwner>(state);
    UnblockOnExit unblock{state};
    state->entered.wait();
    REQUIRE(slot.value().retain(owner));
    REQUIRE_FALSE(duplicate.value().retain(owner));
    REQUIRE(duplicate.value().valid());
    owner.reset();
    state->acknowledged = true;
    unblock.release();
    while (!state->finished)
        std::this_thread::yield();
    std::atomic<std::size_t> collected{};
    std::jthread first([&] { collected += supervisor.collect(); });
    std::jthread second([&] { collected += supervisor.collect(); });
    first.join();
    second.join();
    REQUIRE(collected == 1);
    REQUIRE(state->joins == 1);
    REQUIRE(state->destroyed);
}
