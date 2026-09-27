#include "catch_amalgamated.hpp"
#include "flamoris/runtime/inference.hpp"
#include <atomic>
using namespace flamoris::runtime;
using namespace std::chrono_literals;
namespace {
struct PartialComputeState {
    std::atomic<unsigned> close_calls{0};
    std::atomic<bool> destroyed{false};
};
class PartialCompute final : public ComputeImplementation {
    std::shared_ptr<PartialComputeState> state_;
    std::unique_ptr<ComputeImplementation> cpu_ = make_cpu_compute();

  public:
    explicit PartialCompute(std::shared_ptr<PartialComputeState> state)
        : state_(std::move(state)) {}
    ~PartialCompute() override { state_->destroyed = true; }
    const ComputeDevice &device() const noexcept override { return cpu_->device(); }
    Result<std::vector<float>> matvec(std::span<const float>, std::size_t, std::size_t,
                                      std::span<const float>) override {
        return Result<std::vector<float>>::failure(
            ErrorEnvelope::make(ErrorCode::native_compute_unavailable));
    }
    Result<std::vector<float>> attention(std::span<const float>, std::span<const float>,
                                         std::span<const float>, std::size_t,
                                         std::size_t) override {
        return Result<std::vector<float>>::failure(
            ErrorEnvelope::make(ErrorCode::native_compute_unavailable));
    }
    ComputeReceipt receipt() const noexcept override { return {0, 0, 0, true}; }
    Result<void> synchronize() override { return Result<void>::success(); }
    Result<void> close() override {
        if (++state_->close_calls == 1)
            return Result<void>::failure(
                ErrorEnvelope::make(ErrorCode::cleanup_failed, ErrorStage::cleanup));
        return Result<void>::success();
    }
};
NativeWorkerConfig config() {
    NativeWorkerConfig value;
    value.registered_artifact =
        std::filesystem::path(FLAMORIS_SOURCE_DIR) / "fixtures/native/tiny-causal-v1.bin";
    value.artifact_sha256 = "5cd5a0bacb4cce1efd801fe2a4d7c1347467538ef6e393b37d955d2499ef74e2";
    value.prompt = "日本語";
    value.options.max_output_tokens = 16;
    value.options.sampling.literal_grammar = "😀ok";
    return value;
}
} // namespace
TEST_CASE("B-REAL02 B-REAL03 B-NATIVE01 worker owns native load segments and acknowledged release",
          "[native][cpu]") {
    auto worker = ThreadNativeWorker::create(config());
    REQUIRE(worker);
    REQUIRE(worker.value()->idle());
    DispatchTicket ticket{JobId{RunId{RuntimeInstanceId{2, 3}, 1}, 1}, 1, 1};
    std::uint64_t generation = 0;
    auto run = [&](NativeOperation operation, NativeCommand payload = {}, bool rejected = false) {
        payload.ticket = ticket;
        payload.operation = operation;
        payload.operation_generation = ++generation;
        REQUIRE(worker.value()->submit(std::move(payload)));
        auto observation = worker.value()->wait_take(5s);
        REQUIRE(observation);
        if (rejected) {
            REQUIRE(observation->error);
            REQUIRE(observation->control_rejected);
        } else
            REQUIRE_FALSE(observation->error);
        REQUIRE(observation->quiescent);
        return *observation;
    };
    auto loaded = run(NativeOperation::load);
    REQUIRE(loaded.model_bytes > 0);
    REQUIRE(loaded.state_bytes == 131072);
    REQUIRE_FALSE(worker.value()->close());
    while (loaded.state.stage == NativeStage::prefill)
        loaded = run(NativeOperation::step);
    loaded = run(NativeOperation::step);
    REQUIRE(loaded.segment.text.empty());
    const auto before_rejected_version = loaded.state.version;
    NativeCommand invalid;
    invalid.input = "\xff";
    invalid.injection_command = 4;
    auto rejected_injection = run(NativeOperation::inject, invalid, true);
    REQUIRE(rejected_injection.state.version == before_rejected_version);
    REQUIRE(rejected_injection.state.valid);
    auto rejected_graceful = run(NativeOperation::graceful_stop, {}, true);
    REQUIRE(rejected_graceful.state.version == before_rejected_version);
    REQUIRE(rejected_graceful.state.stage == NativeStage::decode);
    auto paused = run(NativeOperation::pause);
    REQUIRE(paused.state.paused);
    NativeCommand resume;
    resume.resume_pins = paused.state.pins;
    resume.suspension_generation = paused.state.suspension_generation;
    run(NativeOperation::resume, resume);
    std::string output;
    while (loaded.state.stage != NativeStage::completing) {
        loaded = run(NativeOperation::step);
        output += loaded.segment.text;
    }
    REQUIRE(output == "😀ok");
    auto released = run(NativeOperation::release);
    REQUIRE(released.release.released_state_bytes > 0);
    REQUIRE(released.model_allocation_released);
    REQUIRE(worker.value()->close());
    auto second = ThreadNativeWorker::create(config());
    REQUIRE(second);
    REQUIRE(second.value()->close());
}
TEST_CASE("B-REAL01 worker failed artifact load returns bounded failure without execution",
          "[native][cpu]") {
    auto value = config();
    value.artifact_sha256 = std::string(64, '0');
    auto worker = ThreadNativeWorker::create(std::move(value));
    REQUIRE(worker);
    NativeCommand command;
    command.operation = NativeOperation::load;
    command.operation_generation = 1;
    REQUIRE(worker.value()->submit(command));
    auto observation = worker.value()->wait_take(5s);
    REQUIRE(observation);
    REQUIRE(observation->error);
    REQUIRE(observation->error->code() == ErrorCode::unsupported_model);
    REQUIRE(observation->quiescent);
    REQUIRE(observation->state_bytes == 0);
    REQUIRE(worker.value()->close());
}
TEST_CASE("B-REAL03 partially initialized compute remains owned across rejected physical release",
          "[native][cpu]") {
    auto state = std::make_shared<PartialComputeState>();
    auto worker = ThreadNativeWorker::create(config(), [state](const NativeWorkerConfig &) {
        return ComputePreparation{
            std::make_unique<PartialCompute>(state),
            ErrorEnvelope::make(ErrorCode::native_compute_unavailable, ErrorStage::execution)};
    });
    REQUIRE(worker);
    std::uint64_t generation = 0;
    auto run = [&](NativeOperation operation) {
        NativeCommand command;
        command.operation = operation;
        command.operation_generation = ++generation;
        REQUIRE(worker.value()->submit(command));
        auto observation = worker.value()->wait_take(5s);
        REQUIRE(observation);
        return std::move(*observation);
    };
    auto load = run(NativeOperation::load);
    REQUIRE(load.error);
    REQUIRE(load.state_bytes > 0);
    REQUIRE_FALSE(load.state.valid);
    REQUIRE_FALSE(worker.value()->close());
    REQUIRE_FALSE(state->destroyed);
    REQUIRE_FALSE(run(NativeOperation::stop).error);
    auto failed = run(NativeOperation::release);
    REQUIRE(failed.error);
    REQUIRE_FALSE(failed.release.quiescent);
    REQUIRE(failed.state_bytes == load.state_bytes);
    REQUIRE_FALSE(state->destroyed);
    REQUIRE_FALSE(worker.value()->close());
    auto released = run(NativeOperation::release);
    REQUIRE_FALSE(released.error);
    REQUIRE(released.release.quiescent);
    REQUIRE(released.release.released_state_bytes == load.state_bytes);
    REQUIRE(released.state_bytes == 0);
    REQUIRE(state->destroyed);
    REQUIRE(worker.value()->close());
}
