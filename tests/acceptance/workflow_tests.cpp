#include "catch_amalgamated.hpp"
#include "flamoris/runtime/workflow.hpp"
#include "support/deterministic.hpp"
#include <algorithm>

using namespace flamoris::runtime;
using namespace flamoris::runtime::testing;
using namespace std::chrono_literals;
namespace {
JsonValue literal(std::string text) { return JsonValue::Object{{"literal", std::move(text)}}; }
JsonValue reference(std::string source, std::string name) {
    return JsonValue::Object{{"ref", JsonValue::Object{{"source", std::move(source)},
                                                       {"name", std::move(name)},
                                                       {"path", JsonValue::Array{}}}}};
}
JsonValue leaf(std::string id, JsonValue binding = literal("hello")) {
    return JsonValue::Object{{"id", std::move(id)},
                             {"type", "algorithm.echo"},
                             {"with", JsonValue::Object{{"text", std::move(binding)}}}};
}
JsonValue group(std::string id, std::string policy, JsonValue::Array members) {
    JsonValue::Object configuration{{"members", std::move(members)}, {"timeout_ms", 10000.0}};
    const bool race = policy == "race";
    if (race) {
        configuration["loser_policy"] = "cancel_unfinished";
        configuration["accept"] = JsonValue::Object{{"op", "always"}};
    } else
        configuration["policy"] = policy;
    return JsonValue::Object{{"id", std::move(id)},
                             {"type", race ? "control.race" : "control.join"},
                             {"with", JsonValue::Object{}},
                             {"control", std::move(configuration)}};
}
CapabilitySnapshot registry() {
    CapabilityContract contract(EffectSet::from_mask(1).value());
    contract.identifier = "algorithm.echo";
    contract.version = "1";
    contract.adapter_revision = "test/1";
    contract.pausable = true;
    ValueSchema text;
    text.kind = ValueSchema::Kind::string;
    text.max_bytes = 128;
    contract.input_schema.kind = ValueSchema::Kind::object;
    contract.input_schema.properties["text"] = text;
    contract.input_schema.required.insert("text");
    contract.output_schema = text;
    CapabilitySnapshot result;
    result.capabilities.emplace(contract.identifier, std::move(contract));
    return result;
}
std::shared_ptr<const ExecutionPlan> compile(JsonValue::Array nodes, std::string output_node,
                                             bool retry = false) {
    JsonValue request = JsonValue::Object{
        {"schema_version", "flamoris.submit/1"},
        {"kind", "workflow"},
        {"workflow",
         JsonValue::Object{
             {"schema_version", "flamoris.workflow/0.1"},
             {"workflow", JsonValue::Object{{"id", "test"}}},
             {"inputs", JsonValue::Object{}},
             {"nodes", std::move(nodes)},
             {"edges", JsonValue::Array{}},
             {"outputs", JsonValue::Object{{"result", reference("node", std::move(output_node))}}},
             {"limits", JsonValue::Object{}}}},
        {"input_values", JsonValue::Object{}}};
    auto encoded = canonical_json(request);
    REQUIRE(encoded);
    auto capabilities = registry();
    if (retry) {
        auto &contract = capabilities.capabilities.at("algorithm.echo");
        contract.retry_permitted = true;
        contract.provider_deduplication = true;
        contract.max_attempts = 3;
    }
    auto plan = Compiler{}.compile_submission(encoded.value(), capabilities);
    REQUIRE(plan);
    return plan.value().plan;
}
struct Fixture {
    ManualClock clock;
    std::unique_ptr<RunController> run;
    std::unique_ptr<WorkflowMachine> machine;
    explicit Fixture(std::shared_ptr<const ExecutionPlan> plan) {
        auto created =
            RunController::create(RunId{RuntimeInstanceId{3, 4}, 1}, {}, Deadline::at(60s), clock);
        REQUIRE(created);
        run = std::move(created).value();
        auto workflow = WorkflowMachine::create(std::move(plan), {}, *run, clock);
        REQUIRE(workflow);
        machine = std::move(workflow).value();
        REQUIRE(run->queue(run->root()));
    }
    DispatchTicket dispatch(JobId job) {
        auto result = run->dispatch(job, {true, true, true, true});
        REQUIRE(result);
        return result.value();
    }
    WorkflowAdvance advance(JobId job) {
        auto result = machine->advance(dispatch(job));
        REQUIRE(result);
        return std::move(result).value();
    }
    void terminal(JobId job, JsonValue output, bool notify = true) {
        auto ticket = dispatch(job);
        REQUIRE(run->complete(ticket));
        REQUIRE(run->acknowledge_cleanup(job, {true, false, false, false}));
        REQUIRE(run->finalize(job));
        if (notify)
            REQUIRE(machine->accept_result(job, std::move(output)));
    }
    void terminal_running(DispatchTicket ticket, JsonValue output) {
        REQUIRE(run->complete(ticket));
        REQUIRE(run->acknowledge_cleanup(ticket.job, {true, false, false, false}));
        REQUIRE(run->finalize(ticket.job));
        if (ticket.job != run->root())
            REQUIRE(machine->accept_result(ticket.job, std::move(output)));
    }
};
} // namespace

TEST_CASE(
    "A07 workflow executes compiled dependencies with immutable bindings and stable ownership") {
    Fixture f(compile({leaf("first"), leaf("second", reference("node", "first"))}, "second"));
    auto first = f.advance(f.run->root());
    REQUIRE(first.invocations.size() == 1);
    auto child = first.invocations.front();
    REQUIRE(std::get<JsonValue::Object>(child.input.data).at("text") == JsonValue{"hello"});
    REQUIRE(f.run->job(f.run->root()).value().state == JobState::waiting);
    REQUIRE(f.run->job(child.job).value().parent == f.run->root());
    f.terminal(child.job, "from first");
    REQUIRE(f.run->job(f.run->root()).value().state == JobState::queued);
    auto second = f.advance(f.run->root());
    REQUIRE(second.invocations.size() == 1);
    REQUIRE(std::get<JsonValue::Object>(second.invocations[0].input.data).at("text") ==
            JsonValue{"from first"});
    REQUIRE_FALSE(f.machine->accept_result(child.job, "different late value"));
    f.terminal(second.invocations[0].job, "finished");
    auto ticket = f.dispatch(f.run->root());
    auto completed = f.machine->advance(ticket);
    REQUIRE(completed);
    REQUIRE(completed.value().completed_output ==
            JsonValue{JsonValue::Object{{"result", "finished"}}});
    f.terminal_running(ticket, *completed.value().completed_output);
}

TEST_CASE("A20 A21 nested join Jobs own member subtrees and all-settled returns declared order") {
    Fixture f(compile(
        {group("outer", "all_success", {group("inner", "all_settled", {leaf("a"), leaf("b")})})},
        "outer"));
    auto top = f.advance(f.run->root());
    auto outer = top.invocations[0].job;
    REQUIRE(top.invocations[0].coordinating);
    REQUIRE(f.machine->is_coordinator(outer));
    auto middle = f.advance(outer);
    auto inner = middle.invocations[0].job;
    auto leaves = f.advance(inner);
    REQUIRE(leaves.invocations.size() == 2);
    REQUIRE(f.run->job(inner).value().parent == outer);
    for (const auto &leaf : leaves.invocations)
        REQUIRE(f.run->job(leaf.job).value().parent == inner);
    auto b = leaves.invocations[1].job;
    REQUIRE(f.run->request_stop(b, ErrorCode::upstream_failure));
    REQUIRE(f.run->finalize(b));
    REQUIRE(f.machine->accept_failure(b, ErrorEnvelope::make(ErrorCode::upstream_failure)));
    REQUIRE(f.run->job(inner).value().state == JobState::waiting);
    f.terminal(leaves.invocations[0].job, "A");
    auto ticket = f.dispatch(inner);
    auto result = f.machine->advance(ticket);
    REQUIRE(result);
    const auto &outcomes = std::get<JsonValue::Array>(
        std::get<JsonValue::Object>(result.value().completed_output->data).at("outcomes").data);
    REQUIRE(std::get<JsonValue::Object>(outcomes[0].data).at("value") == JsonValue{"A"});
    REQUIRE(std::get<JsonValue::Object>(outcomes[1].data).at("status") == JsonValue{"failed"});
    f.terminal_running(ticket, *result.value().completed_output);
    REQUIRE(f.run->job(outer).value().state == JobState::queued);
    auto outer_ticket = f.dispatch(outer);
    auto outer_result = f.machine->advance(outer_ticket);
    REQUIRE(outer_result);
    f.terminal_running(outer_ticket, *outer_result.value().completed_output);
    REQUIRE(f.run->job(f.run->root()).value().state == JobState::queued);
}

TEST_CASE(
    "A22 race winner is fixed in declared batch order and loser remains owned until settlement") {
    Fixture f(compile({group("race", "race", {leaf("a"), leaf("b")})}, "race"));
    auto top = f.advance(f.run->root());
    auto race = top.invocations[0].job;
    auto candidates = f.advance(race);
    auto a = candidates.invocations[0].job;
    auto b = candidates.invocations[1].job;
    f.terminal(b, "B", false);
    f.terminal(a, "A", false);
    std::vector<WorkflowOutcome> simultaneous{{b, JsonValue{"B"}, {}}, {a, JsonValue{"A"}, {}}};
    REQUIRE(f.machine->accept_batch(simultaneous));
    auto ticket = f.dispatch(race);
    auto result = f.machine->advance(ticket);
    REQUIRE(result);
    REQUIRE(std::get<JsonValue::Object>(result.value().completed_output->data).at("participant") ==
            JsonValue{"a"});
    std::size_t winners = 0;
    for (const auto &g : f.run->events())
        for (const auto &e : g.events)
            if (e.kind == "race.winner_selected")
                ++winners;
    REQUIRE(winners == 1);
    REQUIRE(f.machine->accept_result(b, "B"));
    f.terminal_running(ticket, *result.value().completed_output);
}

TEST_CASE("A23 race resumes coordinator but cannot terminalize before in-flight loser stops") {
    Fixture f(compile({group("race", "race", {leaf("a"), leaf("b")})}, "race"));
    auto race = f.advance(f.run->root()).invocations[0].job;
    auto candidates = f.advance(race);
    auto a = candidates.invocations[0].job;
    auto b = candidates.invocations[1].job;
    auto loser = f.dispatch(b);
    f.terminal(a, "A");
    REQUIRE(f.run->job(b).value().state == JobState::cancelling);
    auto parent = f.dispatch(race);
    auto result = f.machine->advance(parent);
    REQUIRE(result);
    REQUIRE(f.run->complete(parent));
    REQUIRE(f.run->acknowledge_cleanup(race, {true, false, false, false}));
    REQUIRE_FALSE(f.run->finalize(race));
    REQUIRE(f.run->observe_stopped(loser, {true, false, ExternalOutcome::unknown}));
    REQUIRE(f.run->acknowledge_cleanup(b, {true, false, false, false}));
    REQUIRE(f.run->finalize(b));
    REQUIRE(f.machine->accept_failure(b, ErrorEnvelope::make(ErrorCode::outcome_unknown)));
    REQUIRE(f.run->finalize(race));
    REQUIRE(f.machine->accept_result(race, *result.value().completed_output));
    REQUIRE(f.run->job(race).value().state == JobState::succeeded);
    REQUIRE(f.run->job(b).value().state == JobState::failed);
}

TEST_CASE("A20 required child failure cancels siblings without binding a missing result") {
    Fixture f(compile({group("all", "all_success", {leaf("a"), leaf("b")})}, "all"));
    auto parent = f.advance(f.run->root()).invocations[0].job;
    auto children = f.advance(parent);
    auto a = children.invocations[0].job;
    auto b = children.invocations[1].job;
    REQUIRE(f.run->request_stop(a, ErrorCode::upstream_failure));
    REQUIRE(f.run->finalize(a));
    REQUIRE(f.machine->accept_failure(a, ErrorEnvelope::make(ErrorCode::upstream_failure)));
    REQUIRE(f.run->job(parent).value().state == JobState::finalizing);
    REQUIRE(f.run->job(b).value().state == JobState::finalizing);
    REQUIRE_FALSE(f.run->job(parent).value().continuation);
    REQUIRE_FALSE(f.run->dispatch(parent, {true, true, true, true}));
}

TEST_CASE("A16 result while coordinator targeted-paused cannot auto-resume") {
    Fixture f(compile({leaf("a")}, "a"));
    auto child = f.advance(f.run->root()).invocations[0].job;
    REQUIRE(f.run->pause_job(f.run->root(), 9));
    f.terminal(child, "A");
    REQUIRE(f.run->job(f.run->root()).value().state == JobState::paused);
    REQUIRE(f.run->job(f.run->root()).value().wait_satisfied);
    REQUIRE(f.run->resume_job(f.run->root(), 9, true));
    REQUIRE(f.run->job(f.run->root()).value().state == JobState::queued);
}

TEST_CASE(
    "A07 bounded compiled fragment executes beneath suspended native owner without a new Run") {
    auto fragment = compile({leaf("tool")}, "tool");
    ManualClock clock;
    auto created =
        RunController::create(RunId{RuntimeInstanceId{3, 5}, 1}, {}, Deadline::at(60s), clock);
    REQUIRE(created);
    auto run = std::move(created).value();
    REQUIRE(run->queue(run->root()));
    auto native = run->dispatch(run->root(), {true, true, true, true});
    REQUIRE(native);
    ResumePayload preserved;
    preserved.state_reference = 81;
    preserved.state_version = 12;
    const std::vector<ChildSpec> fragment_owner{{Deadline::at(10s), true}};
    auto spawned =
        run->spawn_and_suspend(native.value(), fragment_owner, std::move(preserved), {true, true});
    REQUIRE(spawned);
    auto owner = spawned.value().children[0];
    auto coordinated = WorkflowMachine::create_at(owner, fragment, {}, *run, clock);
    REQUIRE(coordinated);
    auto machine = std::move(coordinated).value();
    REQUIRE(run->queue(owner));
    auto started = run->dispatch(owner, {true, true, true, true});
    REQUIRE(started);
    auto work = machine->advance(started.value());
    REQUIRE(work);
    auto leaf_job = work.value().invocations[0].job;
    REQUIRE(run->job(leaf_job).value().parent == owner);
    REQUIRE(run->job(owner).value().parent == run->root());
    auto tool = run->dispatch(leaf_job, {true, true, true, true});
    REQUIRE(tool);
    REQUIRE(run->complete(tool.value()));
    REQUIRE(run->acknowledge_cleanup(leaf_job, {true, false, false, false}));
    REQUIRE(run->finalize(leaf_job));
    REQUIRE(machine->accept_result(leaf_job, "tool result"));
    auto resumed = run->dispatch(owner, {true, true, true, true});
    REQUIRE(resumed);
    auto done = machine->advance(resumed.value());
    REQUIRE(done);
    REQUIRE(done.value().completed_output ==
            JsonValue{JsonValue::Object{{"result", "tool result"}}});
    REQUIRE(run->complete(resumed.value()));
    REQUIRE(run->acknowledge_cleanup(owner, {true, false, false, false}));
    REQUIRE(run->finalize(owner));
    REQUIRE(run->job(run->root()).value().state == JobState::waiting);
    REQUIRE(run->wake(run->root(), spawned.value().suspension_generation));
    auto parent = run->job(run->root()).value();
    REQUIRE(parent.pending_resume);
    REQUIRE(parent.state_reference == 81);
    REQUIRE(parent.state_version == 12);
    REQUIRE(parent.id == native.value().job);
    REQUIRE(parent.attempt == native.value().attempt);
}

TEST_CASE("B-RETRY01 immutable invocation preserves compiled retry budget and backoff") {
    auto node = leaf("retry");
    std::get<JsonValue::Object>(node.data)["retry"] =
        JsonValue::Object{{"max_attempts", 2.0}, {"backoff_ms", 25.0}};
    Fixture f(compile({node}, "retry", true));
    auto advanced = f.advance(f.run->root());
    REQUIRE(advanced.invocations.size() == 1);
    REQUIRE(advanced.invocations[0].max_attempts == 2);
    REQUIRE(advanced.invocations[0].backoff_ms == 25);
    auto frozen = f.machine->invocation(advanced.invocations[0].job);
    REQUIRE(frozen);
    REQUIRE(frozen->max_attempts == 2);
    REQUIRE(frozen->backoff_ms == 25);
}

TEST_CASE("A20 required child deadline propagates to waiting parent before its later deadline") {
    auto short_child = leaf("a");
    std::get<JsonValue::Object>(short_child.data)["timeout_ms"] = 5000.0;
    Fixture f(compile({group("all", "all_success", {short_child, leaf("b")})}, "all"));
    auto parent = f.advance(f.run->root()).invocations[0].job;
    auto children = f.advance(parent);
    auto first = children.invocations[0].job;
    auto second = children.invocations[1].job;
    REQUIRE(f.clock.advance(5s));
    REQUIRE(f.run->check_deadlines());
    REQUIRE(f.run->job(parent).value().state == JobState::waiting);
    REQUIRE(f.run->job(first).value().state == JobState::finalizing);
    REQUIRE(f.run->finalize(first));
    REQUIRE(f.machine->accept_failure(first, ErrorEnvelope::make(ErrorCode::job_timeout)));
    REQUIRE(f.run->job(parent).value().error == ErrorCode::job_timeout);
    REQUIRE(f.run->job(second).value().state == JobState::finalizing);
    REQUIRE(f.run->job(f.run->root()).value().deadline.time() == 60s);
    REQUIRE(f.clock.now() < f.run->job(parent).value().deadline.time());
    REQUIRE(f.run->finalize(second));
    REQUIRE(f.run->acknowledge_cleanup(parent, {true, false, false, false}));
    REQUIRE(f.run->finalize(parent));
    REQUIRE(f.machine->accept_failure(parent, ErrorEnvelope::make(ErrorCode::job_timeout)));
    REQUIRE(f.run->job(f.run->root()).value().state == JobState::finalizing);
    REQUIRE(f.run->job(f.run->root()).value().error == ErrorCode::job_timeout);
    REQUIRE(f.clock.now() < f.run->job(f.run->root()).value().deadline.time());
}
