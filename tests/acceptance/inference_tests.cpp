#include "catch_amalgamated.hpp"
#include "flamoris/runtime/inference.hpp"
#include "support/deterministic.hpp"
using namespace flamoris::runtime;
using namespace flamoris::runtime::testing;
using namespace std::chrono_literals;
namespace {
constexpr DispatchChecks allowed{true, true, true, true};
class ScriptedWorker final : public NativeWorkerPort {
  public:
    std::optional<NativeCommand> command;
    std::optional<NativeObservation> response;
    std::size_t dispatches{0};
    std::uint64_t version{0};
    Result<void> submit(NativeCommand value) override {
        if (command || response)
            return Result<void>::failure(ErrorEnvelope::make(ErrorCode::resource_unavailable));
        command = std::move(value);
        ++dispatches;
        return Result<void>::success();
    }
    std::optional<NativeObservation> take() override {
        auto value = std::move(response);
        response.reset();
        return value;
    }
    bool idle() const noexcept override { return !command && !response; }
    Result<void> close() override {
        return idle() ? Result<void>::success()
                      : Result<void>::failure(ErrorEnvelope::make(ErrorCode::cleanup_timeout));
    }
    void finish(bool complete = false, bool quiescent = true,
                std::optional<ErrorEnvelope> error = {}) {
        REQUIRE(command);
        NativeObservation value;
        value.ticket = command->ticket;
        value.operation_generation = command->operation_generation;
        value.operation = command->operation;
        value.quiescent = quiescent;
        value.error = error;
        if (!error && (command->operation == NativeOperation::step ||
                       command->operation == NativeOperation::inject))
            ++version;
        value.state.version = version;
        value.state.pins = {"m", "p", "t", "profile", "cpu"};
        value.state.paused = command->operation == NativeOperation::pause;
        value.state.suspension_generation = value.state.paused ? 1 : 0;
        value.segment = {version, 1, 65, "A", quiescent, true, complete};
        value.release = {17, 4096, quiescent};
        response = std::move(value);
        command.reset();
    }
};
struct Fixture {
    ManualClock clock;
    std::unique_ptr<RunController> controller;
    ScriptedWorker *script;
    std::unique_ptr<InferenceMachine> machine;
    DispatchTicket ticket;
    Fixture() {
        auto run = RunController::create(RunId{RuntimeInstanceId{9, 8}, 1}, {}, Deadline::at(100ns),
                                         clock, true);
        REQUIRE(run);
        controller = std::move(run).value();
        REQUIRE(controller->queue(controller->root()));
        auto started = controller->dispatch(controller->root(), allowed);
        REQUIRE(started);
        ticket = started.value();
        auto worker = std::make_unique<ScriptedWorker>();
        script = worker.get();
        machine = std::make_unique<InferenceMachine>(std::move(worker), 44);
    }
    JobSnapshot job() { return controller->job(ticket.job).value(); }
    NativeObservation accept(bool complete = false, bool quiescent = true) {
        script->finish(complete, quiescent);
        auto observation = machine->take();
        REQUIRE(observation);
        REQUIRE(machine->accept(*controller, *observation));
        return *observation;
    }
};
} // namespace
TEST_CASE("C08 A04 A14 native segment dispatch guards pending receipt and cancel fencing") {
    Fixture f;
    REQUIRE_FALSE(f.machine->submit(*f.controller, f.ticket, NativeOperation::load,
                                    {false, true, true, true}));
    REQUIRE(f.script->dispatches == 0);
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::load, allowed));
    f.accept();
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::step, allowed));
    REQUIRE_FALSE(f.machine->submit(*f.controller, f.ticket, NativeOperation::step, allowed));
    REQUIRE(f.controller->request_stop(f.ticket.job));
    auto old = f.accept(true);
    REQUIRE(f.job().state == JobState::cancelling);
    REQUIRE_FALSE(f.machine->accept(*f.controller, old));
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::stop, {}));
    f.accept();
    REQUIRE(f.job().state == JobState::finalizing);
    REQUIRE(f.job().terminal_intent == JobState::cancelled);
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::release, {}));
    f.accept();
    REQUIRE(
        f.job().cleanup_pending); // Only ResourceManager/root can acknowledge physical accounting.
    REQUIRE(f.controller->acknowledge_cleanup(f.ticket.job, {true, false, false, false}));
    REQUIRE(f.controller->finalize(f.ticket.job));
    REQUIRE(f.job().state == JobState::cancelled);
    REQUIRE_FALSE(f.machine->submit(*f.controller, f.ticket, NativeOperation::step, allowed));
}
TEST_CASE("C08 A08 A09 A18 state continuation and fresh native resume ticket") {
    Fixture f;
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::load, allowed));
    f.accept();
    REQUIRE(f.controller->pause_job(f.ticket.job, 11));
    REQUIRE_FALSE(f.machine->submit(*f.controller, f.ticket, NativeOperation::step, allowed));
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::pause, allowed));
    auto paused = f.accept();
    REQUIRE(f.job().state == JobState::paused);
    REQUIRE(f.job().continuation);
    REQUIRE(f.job().state_reference == 44);
    REQUIRE_FALSE(f.machine->submit(*f.controller, f.ticket, NativeOperation::step, allowed));
    REQUIRE(f.controller->resume_job(f.ticket.job, 11, true));
    REQUIRE(f.job().pending_resume);
    auto resumed = f.controller->dispatch(f.ticket.job, allowed);
    REQUIRE(resumed);
    NativeCommand payload;
    payload.resume_pins = paused.state.pins;
    payload.suspension_generation = 1;
    REQUIRE_FALSE(
        f.machine->submit(*f.controller, f.ticket, NativeOperation::resume, allowed, payload));
    f.ticket = resumed.value();
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::resume, allowed, payload));
    f.accept();
    REQUIRE(f.job().state == JobState::running);
    REQUIRE_FALSE(f.job().continuation);
}
TEST_CASE(
    "C08 A16 A28 deadline wins pending completion and unsafe quiescence blocks finalization") {
    Fixture f;
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::step, allowed));
    REQUIRE(f.clock.advance(100ns));
    f.accept(true);
    REQUIRE(f.job().state == JobState::cancelling);
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::stop, {}));
    f.script->finish(false, false);
    auto unsafe = f.machine->take();
    REQUIRE(unsafe);
    REQUIRE_FALSE(f.machine->accept(*f.controller, *unsafe));
    REQUIRE(f.job().state == JobState::cancelling);
    REQUIRE_FALSE(f.controller->finalize(f.ticket.job));
}
TEST_CASE("C08 B-CALL01 invalid generation cannot consume reserved native completion") {
    Fixture f;
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::step, allowed));
    f.script->finish();
    auto receipt = f.machine->take();
    REQUIRE(receipt);
    auto stale = *receipt;
    ++stale.operation_generation;
    REQUIRE_FALSE(f.machine->accept(*f.controller, stale));
    REQUIRE(f.machine->pending());
    REQUIRE(f.machine->accept(*f.controller, *receipt));
    REQUIRE_FALSE(f.machine->pending());
    REQUIRE_FALSE(f.machine->accept(*f.controller, *receipt));
}
TEST_CASE("C08 B-INPUT01 rejected input and unsupported graceful stop preserve healthy Job") {
    Fixture f;
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::load, allowed));
    f.accept();
    const auto watermark = f.controller->snapshot().watermark;
    for (auto operation : {NativeOperation::inject, NativeOperation::graceful_stop}) {
        REQUIRE(f.machine->submit(*f.controller, f.ticket, operation, allowed));
        f.script->finish(false, true, ErrorEnvelope::make(ErrorCode::invalid_request));
        auto rejected = f.machine->take();
        REQUIRE(rejected);
        rejected->control_rejected = true;
        REQUIRE(f.machine->accept(*f.controller, *rejected));
        REQUIRE(f.job().state == JobState::running);
        REQUIRE_FALSE(f.job().terminal_intent);
        REQUIRE(f.controller->snapshot().watermark == watermark);
    }
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::step, allowed));
    f.accept();
    REQUIRE(f.job().state == JobState::running);
}
TEST_CASE("C08 A07 A08 A40 native pause atomically creates admitted child group") {
    Fixture f;
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::load, allowed));
    f.accept();
    REQUIRE(f.machine->submit(*f.controller, f.ticket, NativeOperation::pause, allowed));
    f.script->finish();
    auto receipt = f.machine->take();
    REQUIRE(receipt);
    const std::array<ChildSpec, 1> specs{ChildSpec{Deadline::at(99ns), true}};
    f.controller->fail_next_preparation();
    REQUIRE_FALSE(f.machine->accept_with_children(*f.controller, *receipt, specs));
    REQUIRE(f.machine->pending());
    REQUIRE(f.controller->snapshot().jobs.size() == 1);
    REQUIRE(f.job().state == JobState::running);
    auto spawned = f.machine->accept_with_children(*f.controller, *receipt, specs);
    REQUIRE(spawned);
    REQUIRE_FALSE(f.machine->pending());
    REQUIRE(spawned.value().children.size() == 1);
    REQUIRE(f.job().state == JobState::waiting);
    REQUIRE(f.job().continuation);
    REQUIRE(f.job().state_reference == 44);
    REQUIRE_FALSE(f.job().wait_satisfied);
    REQUIRE_FALSE(f.machine->accept_with_children(*f.controller, *receipt, specs));
}
