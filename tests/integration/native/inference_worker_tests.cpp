#include "catch_amalgamated.hpp"
#include "flamoris/runtime/inference.hpp"
using namespace flamoris::runtime;
using namespace std::chrono_literals;
namespace {
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
