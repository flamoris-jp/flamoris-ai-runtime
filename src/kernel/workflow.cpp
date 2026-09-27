#include "flamoris/runtime/workflow.hpp"
#include "flamoris/runtime/observation.hpp"
#include <algorithm>
#include <limits>

namespace flamoris::runtime {
namespace {
ErrorEnvelope failure(ErrorCode code) { return ErrorEnvelope::make(code, ErrorStage::execution); }
Result<void> reject(ErrorCode code = ErrorCode::invalid_request) {
    return Result<void>::failure(failure(code));
}
bool control(const PlanStep &step) noexcept { return !step.members.empty(); }
JsonValue error_value(const ErrorEnvelope &error) {
    JsonValue::Array causes;
    for (auto code : error.cause_codes())
        causes.emplace_back(std::string(to_string(code)));
    return JsonValue::Object{
        {"schema_version", std::string(ErrorEnvelope::schema_version)},
        {"code", std::string(to_string(error.code()))},
        {"category", std::string(to_string(error.category()))},
        {"message", std::string(error.message())},
        {"stage", std::string(to_string(error.stage()))},
        {"external_outcome", std::string(to_string(error.external_outcome()))},
        {"retry_disposition", std::string(to_string(error.retry_disposition()))},
        {"cause_codes", std::move(causes)}};
}
} // namespace

struct WorkflowMachine::Impl {
    struct Node {
        const PlanStep *step{};
        std::optional<JobId> job;
        std::optional<WorkflowInvocation> invocation;
        std::optional<JsonValue> result;
        std::optional<ErrorEnvelope> error;
        bool settled{};
    };
    struct Frame {
        JobId owner;
        const PlanStep *group{};
        JsonValue::Object inputs;
        std::vector<Node> nodes;
        std::uint64_t suspension{};
        std::optional<std::size_t> winner;
        std::optional<JsonValue> selected_output;
        bool decision_committed{};
        std::size_t cancellation_cursor{};
        Frame(JobId job, const PlanStep *source, JsonValue::Object local,
              const std::vector<PlanStep> &steps)
            : owner(job), group(source), inputs(std::move(local)) {
            nodes.reserve(steps.size());
            for (const auto &step : steps)
                nodes.push_back(Node{&step, {}, {}, {}, {}, false});
        }
    };
    std::shared_ptr<const ExecutionPlan> plan;
    RunController &controller;
    MonotonicClock &clock;
    std::vector<std::unique_ptr<Frame>> frames;
    std::uint64_t steps{};
    std::uint64_t output_bytes{};
    Impl(JobId owner, std::shared_ptr<const ExecutionPlan> compiled, JsonValue::Object input,
         RunController &run, MonotonicClock &time)
        : plan(std::move(compiled)), controller(run), clock(time) {
        frames.reserve(static_cast<std::size_t>(plan->limits.max_jobs));
        frames.push_back(std::make_unique<Frame>(owner, nullptr, std::move(input), plan->steps));
    }
    Frame *frame(JobId job) noexcept {
        for (auto &item : frames)
            if (item->owner == job)
                return item.get();
        return nullptr;
    }
    std::pair<Frame *, Node *> node(JobId job) noexcept {
        for (auto &item : frames)
            for (auto &node : item->nodes)
                if (node.job == job)
                    return {item.get(), &node};
        return {nullptr, nullptr};
    }
    bool ready(const Frame &frame, const Node &node) const noexcept {
        if (node.job)
            return false;
        for (const auto &dep : node.step->dependencies) {
            auto previous = std::find_if(frame.nodes.begin(), frame.nodes.end(),
                                         [&](const auto &value) { return value.step->id == dep; });
            if (previous == frame.nodes.end() || !previous->settled || !previous->result)
                return false;
        }
        return true;
    }
    JsonValue::Object outputs(const Frame &frame) const {
        JsonValue::Object result;
        for (const auto &node : frame.nodes)
            if (node.result)
                result.emplace(node.step->id, *node.result);
        return result;
    }
    Result<void> decision_event(Frame &frame, std::string kind, std::uint64_t participant = 0) {
        LifecycleEvent event;
        event.kind = std::move(kind);
        event.job = frame.owner;
        event.generation = participant;
        return controller.commit_observation(EventGroup{0, 0, {std::move(event)}},
                                             EventStorageClass::control);
    }
    Result<void> wake(Frame &frame) {
        auto state = controller.job(frame.owner);
        if (!state)
            return Result<void>::failure(state.error());
        if (state.value().state == JobState::waiting || state.value().state == JobState::paused)
            return controller.wake(frame.owner, frame.suspension);
        if (state.value().state == JobState::queued || state.value().state == JobState::running)
            return Result<void>::success();
        return reject();
    }
    Result<void> progress_decision(Frame &frame) {
        if (frame.winner) {
            while (frame.cancellation_cursor < frame.nodes.size()) {
                const auto index = frame.cancellation_cursor;
                const auto &node = frame.nodes[index];
                if (index != *frame.winner && node.job && !node.settled) {
                    auto stopped = controller.request_stop(*node.job);
                    if (!stopped)
                        return stopped;
                }
                ++frame.cancellation_cursor;
            }
        }
        return wake(frame);
    }
    Result<void> settle(Frame &frame) {
        auto owner = controller.job(frame.owner);
        if (!owner)
            return Result<void>::failure(owner.error());
        if (is_terminal(owner.value().state) || owner.value().state == JobState::cancelling ||
            owner.value().state == JobState::finalizing)
            return Result<void>::success();
        if (frame.decision_committed)
            return progress_decision(frame);
        const auto policy = frame.group ? frame.group->group_policy : "all_success";
        if (policy == "race") {
            for (std::size_t i = 0; i < frame.nodes.size(); ++i) {
                auto &node = frame.nodes[i];
                if (!node.settled || !node.result ||
                    !evaluate_acceptance(*frame.group->acceptance, *node.result))
                    continue;
                JsonValue result =
                    JsonValue::Object{{"participant", node.step->id}, {"value", *node.result}};
                auto emitted = decision_event(frame, "race.winner_selected",
                                              static_cast<std::uint64_t>(i + 1));
                if (!emitted)
                    return emitted;
                frame.winner = i;
                frame.selected_output = std::move(result);
                frame.decision_committed = true;
                return progress_decision(frame);
            }
            const bool all = std::all_of(frame.nodes.begin(), frame.nodes.end(),
                                         [](const auto &node) { return node.settled; });
            if (all)
                return controller.request_stop(frame.owner, ErrorCode::race_no_acceptable_result);
            return Result<void>::success();
        }
        if (policy != "all_settled")
            for (const auto &node : frame.nodes)
                if (node.settled && node.error) {
                    const auto code = node.error->code();
                    return controller.request_stop(frame.owner,
                                                   code == ErrorCode::job_cancelled ||
                                                           code == ErrorCode::run_cancelled
                                                       ? ErrorCode::upstream_failure
                                                       : code);
                }
        const bool active_done =
            std::all_of(frame.nodes.begin(), frame.nodes.end(),
                        [](const auto &node) { return !node.job || node.settled; });
        if (!active_done)
            return Result<void>::success();
        const bool all = std::all_of(frame.nodes.begin(), frame.nodes.end(),
                                     [](const auto &node) { return node.settled; });
        if (frame.group && all) {
            JsonValue::Array collection;
            collection.reserve(frame.nodes.size());
            for (const auto &node : frame.nodes) {
                if (policy == "all_settled") {
                    if (node.result)
                        collection.emplace_back(
                            JsonValue::Object{{"status", "succeeded"}, {"value", *node.result}});
                    else {
                        auto state = controller.job(*node.job);
                        collection.emplace_back(JsonValue::Object{
                            {"status",
                             state.value().state == JobState::cancelled ? "cancelled" : "failed"},
                            {"error", error_value(*node.error)}});
                    }
                } else
                    collection.push_back(*node.result);
            }
            JsonValue result = JsonValue::Object{
                {policy == "all_settled" ? "outcomes" : "results", std::move(collection)}};
            auto emitted = decision_event(frame, "group.completed");
            if (!emitted)
                return emitted;
            frame.selected_output = std::move(result);
            frame.decision_committed = true;
        }
        return wake(frame);
    }
};

WorkflowMachine::WorkflowMachine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
WorkflowMachine::~WorkflowMachine() = default;
Result<std::unique_ptr<WorkflowMachine>>
WorkflowMachine::create(std::shared_ptr<const ExecutionPlan> plan, JsonValue::Object inputs,
                        RunController &controller, MonotonicClock &clock) {
    return create_at(controller.root(), std::move(plan), std::move(inputs), controller, clock);
}
Result<std::unique_ptr<WorkflowMachine>>
WorkflowMachine::create_at(JobId owner, std::shared_ptr<const ExecutionPlan> plan,
                           JsonValue::Object inputs, RunController &controller,
                           MonotonicClock &clock) {
    auto record = controller.job(owner);
    if (!record ||
        (record.value().state != JobState::created && record.value().state != JobState::queued))
        return Result<std::unique_ptr<WorkflowMachine>>::failure(
            failure(ErrorCode::invalid_request));
    if (!plan || plan->single_root_inference || plan->steps.empty() || plan->limits.max_jobs == 0 ||
        plan->limits.max_jobs > 65536 || plan->limits.max_control_steps == 0)
        return Result<std::unique_ptr<WorkflowMachine>>::failure(
            failure(ErrorCode::invalid_workflow));
    for (const auto &[name, schema] : plan->inputs) {
        auto value = inputs.find(name);
        if (value == inputs.end() || !validate_value(value->second, schema))
            return Result<std::unique_ptr<WorkflowMachine>>::failure(
                failure(ErrorCode::invalid_request));
    }
    for (const auto &[name, value] : inputs) {
        (void)value;
        if (!plan->inputs.contains(name))
            return Result<std::unique_ptr<WorkflowMachine>>::failure(
                failure(ErrorCode::invalid_request));
    }
    return Result<std::unique_ptr<WorkflowMachine>>::success(
        std::unique_ptr<WorkflowMachine>(new WorkflowMachine(
            std::make_unique<Impl>(owner, std::move(plan), std::move(inputs), controller, clock))));
}
bool WorkflowMachine::is_coordinator(JobId job) const noexcept {
    return impl_->frame(job) != nullptr;
}
std::uint64_t WorkflowMachine::control_steps() const noexcept { return impl_->steps; }
std::optional<WorkflowInvocation> WorkflowMachine::invocation(JobId job) const {
    auto [frame, node] = impl_->node(job);
    (void)frame;
    return node ? node->invocation : std::nullopt;
}
Result<WorkflowAdvance> WorkflowMachine::advance(DispatchTicket ticket) {
    auto deny = [](ErrorCode code) { return Result<WorkflowAdvance>::failure(failure(code)); };
    auto *frame = impl_->frame(ticket.job);
    if (!frame || !impl_->controller.ticket_is_current(ticket))
        return deny(ErrorCode::invalid_request);
    const auto owner = impl_->controller.job(ticket.job);
    if (!owner || owner.value().state != JobState::running)
        return deny(ErrorCode::invalid_request);
    if (owner.value().deadline.expired(impl_->clock.now())) {
        auto stopped = impl_->controller.request_stop(ticket.job, ErrorCode::job_timeout);
        (void)stopped;
        return deny(ErrorCode::job_timeout);
    }
    if (impl_->steps >= impl_->plan->limits.max_control_steps)
        return deny(ErrorCode::budget_exceeded);
    ++impl_->steps;
    if (frame->selected_output)
        return Result<WorkflowAdvance>::success(WorkflowAdvance{{}, frame->selected_output});
    auto prior = impl_->outputs(*frame);
    const bool all = std::all_of(frame->nodes.begin(), frame->nodes.end(),
                                 [](const auto &node) { return node.settled; });
    if (all) {
        if (frame->group)
            return deny(ErrorCode::invariant_violation);
        JsonValue::Object output;
        for (const auto &[name, binding] : impl_->plan->outputs) {
            auto value = resolve_binding(binding, frame->inputs, prior);
            if (!value)
                return Result<WorkflowAdvance>::failure(value.error());
            output.emplace(name, std::move(value).value());
        }
        return Result<WorkflowAdvance>::success(WorkflowAdvance{{}, JsonValue{std::move(output)}});
    }
    std::vector<std::size_t> indices;
    std::vector<ChildSpec> specs;
    WorkflowAdvance output;
    std::vector<std::unique_ptr<Impl::Frame>> new_frames;
    indices.reserve(frame->nodes.size());
    specs.reserve(frame->nodes.size());
    output.invocations.reserve(frame->nodes.size());
    new_frames.reserve(frame->nodes.size());
    for (std::size_t i = 0; i < frame->nodes.size(); ++i) {
        const auto &node = frame->nodes[i];
        if (!impl_->ready(*frame, node))
            continue;
        JsonValue::Object input;
        for (const auto &[name, binding] : node.step->inputs) {
            auto value = resolve_binding(binding, frame->inputs, prior);
            if (!value)
                return Result<WorkflowAdvance>::failure(value.error());
            input.emplace(name, std::move(value).value());
        }
        auto duration = std::chrono::milliseconds(node.step->timeout_ms);
        auto requested = Deadline::after(impl_->clock.now(), duration);
        if (!requested)
            return deny(ErrorCode::invalid_workflow);
        auto deadline =
            Deadline::at(std::min(requested.value().time(), owner.value().deadline.time()));
        specs.push_back({deadline, control(*node.step) || node.step->pause_supported});
        indices.push_back(i);
        output.invocations.push_back({{},
                                      node.step->structural_path,
                                      node.step->capability_pin,
                                      JsonValue{input},
                                      node.step->output_schema,
                                      node.step->effects,
                                      deadline,
                                      control(*node.step),
                                      node.step->max_attempts,
                                      node.step->backoff_ms,
                                      node.step->child_envelope});
        if (control(*node.step))
            new_frames.push_back(std::make_unique<Impl::Frame>(JobId{}, node.step, std::move(input),
                                                               node.step->members));
        else
            new_frames.push_back(nullptr);
    }
    if (indices.empty())
        return deny(ErrorCode::invalid_workflow);
    if (new_frames.size() > impl_->plan->limits.max_jobs - impl_->frames.size())
        return deny(ErrorCode::budget_exceeded);
    // Every fallible immutable input/frame allocation precedes the one controller
    // child registration+suspension commit. The parent never holds a worker slot.
    std::vector<WorkflowInvocation> stored = output.invocations;
    ResumePayload payload;
    auto spawned =
        impl_->controller.spawn_and_suspend(ticket, specs, std::move(payload), {true, true});
    if (!spawned)
        return Result<WorkflowAdvance>::failure(spawned.error());
    frame->suspension = spawned.value().suspension_generation;
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const auto job = spawned.value().children[i];
        auto &node = frame->nodes[indices[i]];
        node.job = job;
        stored[i].job = job;
        node.invocation = std::move(stored[i]);
        output.invocations[i].job = job;
        if (new_frames[i]) {
            new_frames[i]->owner = job;
            impl_->frames.push_back(std::move(new_frames[i]));
        }
        auto queued = impl_->controller.queue(job);
        if (!queued) {
            auto stopped = impl_->controller.request_stop(frame->owner, queued.error().code());
            (void)stopped;
            return Result<WorkflowAdvance>::failure(queued.error());
        }
    }
    return Result<WorkflowAdvance>::success(std::move(output));
}
Result<void> WorkflowMachine::accept_result(JobId job, JsonValue result) {
    auto [frame, node] = impl_->node(job);
    if (!node)
        return reject();
    auto state = impl_->controller.job(job);
    if (!state || state.value().state != JobState::succeeded)
        return reject();
    if (node->settled)
        return node->result && *node->result == result ? impl_->settle(*frame) : reject();
    auto valid = validate_value(result, node->step->output_schema);
    if (!valid)
        return Result<void>::failure(valid.error());
    auto encoded = canonical_json(result);
    if (!encoded)
        return Result<void>::failure(encoded.error());
    if (encoded.value().size() > impl_->plan->limits.max_output_bytes - impl_->output_bytes)
        return reject(ErrorCode::result_too_large);
    node->result = std::move(result);
    node->settled = true;
    impl_->output_bytes += encoded.value().size();
    return impl_->settle(*frame);
}
Result<void> WorkflowMachine::accept_failure(JobId job, ErrorEnvelope error) {
    auto [frame, node] = impl_->node(job);
    if (!node)
        return reject();
    auto state = impl_->controller.job(job);
    if (!state || !is_terminal(state.value().state) || state.value().state == JobState::succeeded)
        return reject();
    if (node->settled)
        return impl_->settle(*frame);
    node->error = error;
    node->settled = true;
    return impl_->settle(*frame);
}
Result<void> WorkflowMachine::accept_batch(std::span<const WorkflowOutcome> outcomes) {
    if (outcomes.size() > impl_->plan->limits.max_jobs)
        return reject(ErrorCode::budget_exceeded);
    std::vector<const WorkflowOutcome *> ordered;
    ordered.reserve(outcomes.size());
    for (const auto &outcome : outcomes) {
        if (outcome.result.has_value() == outcome.failure.has_value() ||
            !impl_->node(outcome.job).second)
            return reject();
        for (const auto *previous : ordered)
            if (previous->job == outcome.job)
                return reject();
        ordered.push_back(&outcome);
    }
    // Job allocation follows declared member order inside each simultaneous wave.
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const auto *a, const auto *b) { return a->job < b->job; });
    for (const auto *outcome : ordered) {
        auto accepted = outcome->result ? accept_result(outcome->job, *outcome->result)
                                        : accept_failure(outcome->job, *outcome->failure);
        if (!accepted)
            return accepted;
    }
    return Result<void>::success();
}
} // namespace flamoris::runtime
