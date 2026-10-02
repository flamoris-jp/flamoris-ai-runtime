#include "flamoris/runtime/runtime.hpp"
#include "flamoris/runtime/activation.hpp"
#include "flamoris/runtime/adapter_binding.hpp"
#include "flamoris/runtime/cleanup_observation.hpp"
#include "flamoris/runtime/limits.hpp"
#include "flamoris/runtime/native_registration.hpp"
#include "flamoris/runtime/runtime_residency.hpp"
#include "flamoris/runtime/scheduler.hpp"
#include "flamoris/runtime/workflow.hpp"
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <iterator>
#include <limits>
#include <mutex>
#include <random>
#include <thread>

namespace flamoris::runtime {
namespace {
template <class T> Result<T> failure(ErrorCode c, ErrorStage stage = ErrorStage::admission) {
    return Result<T>::failure(ErrorEnvelope::make(c, stage));
}
std::uint64_t milliseconds(TimePoint time) noexcept {
    return static_cast<std::uint64_t>(std::max<Duration::rep>(0, time.count()) / 1000000);
}
bool done(JobState state) { return is_terminal(state); }
AuthorizationContext attenuate(const AuthorizationContext &ceiling,
                               const AuthorizationContext &current) {
    AuthorizationContext result;
    result.subject = ceiling.subject == current.subject ? ceiling.subject : std::string{};
    result.expires_at_ms = std::min(ceiling.expires_at_ms, current.expires_at_ms);
    result.revoked = ceiling.revoked || current.revoked;
    std::set_intersection(ceiling.capabilities.begin(), ceiling.capabilities.end(),
                          current.capabilities.begin(), current.capabilities.end(),
                          std::inserter(result.capabilities, result.capabilities.end()));
    std::set_intersection(ceiling.object_scopes.begin(), ceiling.object_scopes.end(),
                          current.object_scopes.begin(), current.object_scopes.end(),
                          std::inserter(result.object_scopes, result.object_scopes.end()));
    std::set_intersection(ceiling.access.begin(), ceiling.access.end(), current.access.begin(),
                          current.access.end(), std::inserter(result.access, result.access.end()));
    result.permitted_effects = ceiling.permitted_effects & current.permitted_effects;
    for (const auto &claim : ceiling.confirmations)
        for (const auto &presented : current.confirmations)
            if (claim.subject == presented.subject && claim.capability == presented.capability &&
                claim.input_digest == presented.input_digest &&
                claim.effects == presented.effects) {
                auto bounded = claim;
                bounded.expires_at_ms = std::min(claim.expires_at_ms, presented.expires_at_ms);
                result.confirmations.push_back(std::move(bounded));
                break;
            }
    return result;
}
JsonValue::Object arguments(const PlanStep &step, const JsonValue::Object &inputs) {
    JsonValue::Object result;
    for (const auto &[name, binding] : step.inputs) {
        auto value = resolve_binding(binding, inputs, {});
        if (!value)
            throw std::runtime_error("invalid binding");
        result.emplace(name, std::move(value).value());
    }
    return result;
}
JsonValue registration_projection(const RuntimeRegistration &r) {
    JsonValue::Array requirements, limit;
    for (auto value : r.requirements.values)
        requirements.emplace_back(std::to_string(value));
    for (auto value : r.run_resource_limit.values)
        limit.emplace_back(std::to_string(value));
    JsonValue::Object object{{"requirements", std::move(requirements)},
                             {"run_limit", std::move(limit)},
                             {"resource", std::to_string(r.resource.value)}};
    if (r.native) {
        const auto &n = *r.native;
        JsonValue::Array stops;
        for (const auto &stop : n.options.stop_sequences)
            stops.emplace_back(stop);
        object.emplace(
            "native",
            JsonValue::Object{
                {"artifact", n.artifact_sha256},
                {"compute", n.compute_identity},
                {"context_bytes", std::to_string(n.retained_context_bytes)},
                {"device_bound", std::to_string(n.device_memory_bound)},
                {"input_bound", std::to_string(n.options.max_input_bytes)},
                {"output_bound", std::to_string(n.options.max_output_tokens)},
                {"injection", n.options.injection_slot},
                {"partial", n.options.allow_partial_output},
                {"seed", std::to_string(n.options.sampling.seed)},
                {"temperature", static_cast<double>(n.options.sampling.temperature)},
                {"repetition_penalty", static_cast<double>(n.options.sampling.repetition_penalty)},
                {"top_k", std::to_string(n.options.sampling.top_k)},
                {"grammar", n.options.sampling.literal_grammar},
                {"stops", std::move(stops)}});
    }
    return object;
}
Result<void> bind_registration_contracts(CapabilitySnapshot &capabilities,
                                         const std::vector<RuntimeRegistration> &registrations) {
    std::set<std::string> seen;
    std::map<std::string, CapabilityPin> before;
    for (const auto &[id, cap] : capabilities.capabilities) {
        auto pin = fingerprint_capability(cap);
        if (!pin)
            return Result<void>::failure(pin.error());
        before.emplace(id, pin.value());
    }
    for (const auto &r : registrations) {
        if (!seen.insert(r.capability).second)
            return failure<void>(ErrorCode::invalid_request);
        auto cap = capabilities.capabilities.find(r.capability);
        if (cap == capabilities.capabilities.end())
            return failure<void>(ErrorCode::invalid_reference);
        if (r.native) {
            auto valid = validate_native_registration(*r.native, cap->second, r.requirements);
            if (!valid)
                return valid;
        }
        auto digest = domain_digest("flamoris.resource-contract/1\n", registration_projection(r));
        if (!digest)
            return Result<void>::failure(digest.error());
        cap->second.resource_contract_digest = std::move(digest).value();
    }
    // The compiler must never admit a capability that this Runtime cannot dispatch.
    for (const auto &[id, cap] : capabilities.capabilities)
        if (cap.available && !seen.contains(id))
            return failure<void>(ErrorCode::capability_unavailable);
    for (auto &[id, envelope] : capabilities.child_policies) {
        (void)id;
        for (auto &pin : envelope.capabilities) {
            auto old = before.find(pin.identifier);
            if (old == before.end() || old->second != pin)
                return failure<void>(ErrorCode::plan_stale);
            auto updated = fingerprint_capability(capabilities.capabilities.at(pin.identifier));
            if (!updated)
                return Result<void>::failure(updated.error());
            pin = updated.value();
        }
    }
    return Result<void>::success();
}
ModelResidencyKey model_key(const RuntimeRegistration &registration,
                            const CapabilityContract &capability, HostEpoch epoch) {
    JsonValue::Object pins;
    for (const auto &[name, value] : capability.native_pins)
        pins.emplace(name, value);
    auto digest =
        domain_digest("flamoris.residency/1\n",
                      JsonValue::Object{{"pins", std::move(pins)},
                                        {"registration", registration_projection(registration)}});
    if (!digest)
        throw std::runtime_error("invalid model identity");
    ModelResidencyKey key;
    key.resource = registration.resource;
    key.host_epoch = epoch;
    key.worker = registration.worker_generation;
    auto nibble = [](char c) { return static_cast<unsigned>(c <= '9' ? c - '0' : c - 'a' + 10); };
    for (std::size_t i = 0; i < 32; ++i)
        key.configuration_fingerprint[i] = static_cast<std::uint8_t>(
            (nibble(digest.value()[2 * i]) << 4) | nibble(digest.value()[2 * i + 1]));
    return key;
}
} // namespace
struct RuntimeInstance::Impl final : RunObservationLookupPort,
                                     RuntimeRetainedOwner,
                                     RuntimeCleanupEndpoint {
    struct Proposal {
        CompiledSubmission compiled;
        std::uint64_t dispatch_generation;
    };
    struct Driver {
        WorkflowInvocation invocation;
        const RuntimeRegistration *registration{};
        std::optional<ResourceTicket> reservation;
        std::optional<ExecutionLease> lease;
        std::optional<OperationId> release_operation;
        std::optional<OperationId> host_grant;
        std::optional<TimePoint> cleanup_started;
        bool cleanup_transferred{};
        std::optional<DispatchTicket> ticket;
        std::optional<AllocationIdentity> state_allocation, model_allocation;
        std::optional<AllocationIdentity> prepared_state, prepared_model;
        std::optional<ModelResidencyKey> residency_key;
        bool residency_loading{}, residency_attached{};
        std::optional<TimePoint> acquisition_deadline;
        std::optional<NativeObservation> retained_observation;
        std::optional<Proposal> proposal;
        std::unique_ptr<WorkflowMachine> fragment;
        WorkflowMachine *owner_machine{};
        std::optional<JobId> dynamic_parent;
        std::optional<std::string> injection;
        std::uint64_t injection_id{}, child_depth{};
        unsigned close_attempts{};
        std::shared_ptr<const ExecutionPlan> execution_plan;
        std::unique_ptr<InferenceMachine> native;
        std::future<Result<std::unique_ptr<NativeWorkerPort>>> preparing_native;
        std::future<AdapterOutcome> provider;
        std::future<std::unique_ptr<InferenceMachine>> closing_native;
        std::optional<RetryEvidence> retry_evidence;
        std::optional<ErrorEnvelope> retry_error;
        std::optional<TimePoint> retry_not_before;
        bool retry_waiting{};
        std::optional<JsonValue> output;
        std::optional<ErrorEnvelope> error;
        ExternalOutcome outcome{ExternalOutcome::not_applicable};
        std::string text;
        bool native_loaded{}, native_released{}, native_quiescent{}, release_requested{},
            host_released{}, published{}, resuming{};
    };
    struct Run {
        const AuthorizationContext admission_context;
        AuthorizationContext context;
        std::shared_ptr<const ExecutionPlan> plan;
        std::unique_ptr<RunController> controller;
        RunBudget budget;
        std::unique_ptr<WorkflowMachine> workflow;
        std::optional<TimePoint> terminal_since;
        std::map<JobId, Driver> drivers;
        RunResult result;
        explicit Run(AuthorizationContext c, std::shared_ptr<const ExecutionPlan> p,
                     std::unique_ptr<RunController> ctl)
            : admission_context(c), context(std::move(c)), plan(std::move(p)),
              controller(std::move(ctl)), budget(plan->limits) {}
    };
    struct ClaimWait {
        SubmissionTicket ticket;
        std::shared_ptr<std::promise<Result<SubmissionReceipt>>> promise;
        std::shared_future<Result<SubmissionReceipt>> future;
        std::uint64_t admission_claim{};
    };
    RuntimeConfiguration config;
    SubmissionIndex submissions;
    AdmissionGate admission;
    Compiler compiler;
    AuthorizationGate gate;
    AdapterAuthority adapter_authority;
    Scheduler scheduler;
    ControllerCleanupObservationPort cleanup;
    ResourceManager resources;
    RuntimeResidencyPool residency;
    std::map<RunId, std::unique_ptr<Run>> runs;
    std::map<std::string, std::shared_ptr<PersistentAdapterBinding>> adapters;
    std::map<PendingSubmissionId, ClaimWait> pending;
    std::map<LogicalResourceId, HostEnvelope> envelopes;
    CheckedCounter operations;
    std::uint64_t run_retirement_floor{1};
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::function<void()>> normal, mandatory;
    bool stopping{}, accepting{true};
    std::atomic<bool> cleanup_settled{false};
    std::optional<TimePoint> drain_deadline, cleanup_deadline;
    std::thread actor;

    explicit Impl(RuntimeConfiguration value)
        : config(std::move(value)), submissions(config.instance, config.submissions),
          admission(1, config.normal_commands), compiler(config.compiler),
          scheduler(config.max_runs * config.compiler.limits.max_jobs),
          cleanup(*this, *config.clock, 128), resources(config.instance, *config.host, cleanup),
          residency(resources) {
        for (const auto &registration : config.registrations)
            if (registration.provider) {
                auto binding = PersistentAdapterBinding::create(
                    config.capabilities.capabilities.at(registration.capability),
                    registration.provider,
                    std::min<std::size_t>(65536,
                                          config.max_runs * config.compiler.limits.max_attempts));
                if (!binding)
                    throw std::runtime_error("invalid adapter binding");
                adapters.emplace(registration.capability, std::move(binding).value());
            }
    }
    bool cleanup_complete() const noexcept override {
        return cleanup_settled.load(std::memory_order_acquire);
    }
    void close_and_join() noexcept override {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        condition.notify_all();
        if (actor.joinable())
            actor.join();
    }
    ~Impl() {
        if (actor.joinable())
            actor.join();
    }
    RuntimeDrainSnapshot drain_snapshot() const {
        RuntimeDrainSnapshot value;
        value.admission_closed = !accepting;
        value.drain_deadline = drain_deadline;
        value.cleanup_deadline = cleanup_deadline;
        value.drain_expired = drain_deadline && config.clock->now() >= *drain_deadline;
        value.cleanup_expired = cleanup_deadline && config.clock->now() >= *cleanup_deadline;
        value.settled = cleanup_settled.load(std::memory_order_acquire);
        for (const auto &[id, run] : runs) {
            (void)id;
            for (const auto &job : run->controller->snapshot().jobs)
                if (!is_terminal(job.state))
                    ++value.nonterminal_jobs;
            for (const auto &[job, d] : run->drivers) {
                (void)job;
                if (d.native || d.provider.valid() || d.preparing_native.valid() ||
                    d.closing_native.valid())
                    ++value.active_workers;
            }
        }
        for (const auto &[resource, envelope] : envelopes) {
            (void)envelope;
            const auto ledger = resources.snapshot(resource);
            value.unresolved_resources += ledger.allocations + ledger.cleanup_records;
            if (!ledger.reserved.empty() || !ledger.executing.empty() || !ledger.quarantine.empty())
                ++value.unresolved_resources;
        }
        return value;
    }
    Result<void> observe_host_envelope(HostEnvelope value) override {
        return call<void>(
            [this, value] {
                auto result = resources.observe_envelope(value, config.clock->now());
                if (result) {
                    auto old = envelopes.find(value.resource);
                    if (old != envelopes.end() && old->second.epoch != value.epoch)
                        residency.fence(old->second.epoch);
                    envelopes[value.resource] = value;
                }
                return result;
            },
            true);
    }
    Result<void> observe_host_acquisition(HostAcquisition value) override {
        return call<void>([this, value] { return resources.acquired(value); }, true);
    }
    Result<void> observe_host_release(HostReleaseReceipt value) override {
        return call<void>(
            [this, value] { return resources.observe_host_release(value, config.clock->now()); },
            true);
    }
    RunController *find_observation_run(RunId id) noexcept override {
        auto it = runs.find(id);
        return it == runs.end() ? nullptr : it->second->controller.get();
    }
    std::uint64_t now_ms() const noexcept { return milliseconds(config.clock->now()); }
    struct PendingFailureGuard {
        Impl &owner;
        SubmissionTicket ticket;
        ~PendingFailureGuard() noexcept {
            auto it = owner.pending.find(ticket.pending);
            if (it == owner.pending.end())
                return;
            auto error = ErrorEnvelope::make(ErrorCode::internal_error);
            try {
                (void)owner.submissions.reject(ticket, error);
                it->second.promise->set_value(Result<SubmissionReceipt>::failure(error));
                (void)owner.admission.settle_claim(it->second.admission_claim);
                owner.pending.erase(it);
            } catch (...) {
                owner.accepting = false;
            }
        }
    };
    template <class T, class F> Result<T> call(F function, bool required = false) {
        auto promise = std::make_shared<std::promise<Result<T>>>();
        auto result = promise->get_future();
        {
            std::unique_lock lock(mutex);
            if (required)
                condition.wait(
                    lock, [&] { return stopping || mandatory.size() < config.mandatory_commands; });
            auto &queue = required ? mandatory : normal;
            const auto bound = required ? config.mandatory_commands : config.normal_commands;
            if (stopping || queue.size() >= bound)
                return failure<T>(ErrorCode::resource_unavailable);
            queue.emplace_back([promise, function = std::move(function)]() mutable {
                try {
                    promise->set_value(function());
                } catch (...) {
                    promise->set_value(failure<T>(ErrorCode::internal_error));
                }
            });
        }
        condition.notify_one();
        return result.get();
    }
    Result<void> access(const AuthorizationContext &context, const Run &run,
                        AccessSurface surface) const {
        return gate.check_access(context, config.policy, run.context.subject, surface, now_ms());
    }
    const RuntimeRegistration *registration(std::string_view capability) const {
        const auto it =
            std::find_if(config.registrations.begin(), config.registrations.end(),
                         [&](const auto &value) { return value.capability == capability; });
        return it == config.registrations.end() ? nullptr : &*it;
    }
    std::size_t workers() const {
        std::size_t n = 0;
        for (const auto &[id, run] : runs) {
            (void)id;
            for (const auto &[job, d] : run->drivers) {
                (void)job;
                if (d.lease || d.provider.valid() || d.preparing_native.valid())
                    ++n;
            }
        }
        return n;
    }
    Result<AuthorizationDecision> authorize(Run &run, Driver &d, AuthorizationBoundary boundary) {
        if (!d.invocation.capability_pin)
            return failure<AuthorizationDecision>(ErrorCode::invalid_reference);
        AuthorizationRequest request;
        request.boundary = boundary;
        request.capability = d.invocation.capability_pin->identifier;
        request.now_ms = now_ms();
        request.deadline_ms = milliseconds(d.invocation.deadline.time());
        request.concrete_inputs = std::get<JsonValue::Object>(d.invocation.input.data);
        auto job = run.controller->job(d.invocation.job);
        request.cancelled = !job || job.value().state == JobState::cancelling ||
                            job.value().state == JobState::finalizing;
        request.state_accessible =
            !d.native || !d.native->native_state() || d.native->native_state()->valid;
        request.resources_ready =
            !d.lease || (d.native_loaded ? resources.ready_for_use(*d.lease, config.clock->now())
                                         : resources.lease_current(*d.lease, config.clock->now()));
        return gate.check(run.context, config.policy, *d.execution_plan, config.capabilities,
                          request);
    }
    void stop(Run &run, Driver &d, ErrorEnvelope error) {
        d.error = error;
        auto state = run.controller->job(d.invocation.job);
        if (state && !done(state.value().state)) {
            (void)run.controller->request_stop(d.invocation.job, error.code());
            if (d.ticket && !d.native && !d.preparing_native.valid() && !d.provider.valid()) {
                d.native_quiescent = true;
                if (d.residency_loading && d.residency_key && d.reservation) {
                    (void)residency.cancel_loading(*d.residency_key, d.reservation->owner);
                    (void)residency.loading_stopped(*d.residency_key, d.reservation->owner,
                                                    ContainmentProof::worker_quiesced);
                    d.residency_loading = false;
                }
                (void)run.controller->observe_stopped(
                    *d.ticket, {true, false, ExternalOutcome::not_dispatched});
            }
        }
    }
    void enqueue(Run &run, JobId job, const Driver &d) {
        ReadyJob ready;
        ready.job = job;
        ready.priority = 1;
        ready.not_before = d.retry_not_before.value_or(TimePoint{});
        if (d.registration) {
            ready.resource = d.registration->resource;
            ready.requirements = d.registration->requirements;
        }
        (void)scheduler.ready(ready, *run.controller, config.clock->now());
    }
    void request_host_release(Driver &d) {
        if (!d.reservation)
            return;
        if (d.native_quiescent)
            (void)resources.native_quiesced(*d.reservation, ContainmentProof::worker_quiesced, true,
                                            config.clock->now());
        if (resources.host_release_operation(*d.reservation))
            return;
        auto op = operations.next();
        if (!op)
            return;
        // The ledger records the one-use identity before outbound handoff and
        // retains uncertainty even if the configured enqueue port throws.
        d.release_requested = true;
        d.release_operation = OperationId{op.value()};
        (void)resources.request_host_release(*d.reservation, OperationId{op.value()});
    }
    void release_ledger(Driver &d) {
        if (!d.reservation)
            return;
        if (d.native_quiescent && d.release_requested)
            (void)resources.native_quiesced(*d.reservation, ContainmentProof::worker_quiesced, true,
                                            config.clock->now());
        if (!resources.reservation_settled(*d.reservation))
            return;
        d.lease.reset();
        d.reservation.reset();
        d.release_requested = false;
        d.host_released = false;
        d.release_operation.reset();
        d.acquisition_deadline.reset();
    }
    Result<void> add_driver(Run &run, WorkflowInvocation invocation,
                            WorkflowMachine *owner_machine = nullptr) {
        Driver d;
        d.invocation = std::move(invocation);
        d.owner_machine = owner_machine ? owner_machine : run.workflow.get();
        d.execution_plan = run.plan;
        if (!d.invocation.coordinating) {
            if (!d.invocation.capability_pin)
                return failure<void>(ErrorCode::invalid_reference);
            d.registration = registration(d.invocation.capability_pin->identifier);
            if (!d.registration)
                return failure<void>(ErrorCode::capability_unavailable);
        }
        auto job = d.invocation.job;
        auto [it, inserted] = run.drivers.emplace(job, std::move(d));
        if (!inserted)
            return failure<void>(ErrorCode::invariant_violation);
        enqueue(run, job, it->second);
        return Result<void>::success();
    }
    void accept_native(Run &run, Driver &d, NativeObservation observation) {
        auto receipt_budget = run.budget;
        if (observation.operation == NativeOperation::step && !observation.control_rejected) {
            const auto state = run.controller->job(d.invocation.job);
            if (state && state.value().state == JobState::running) {
                if (observation.segment.text.size() >
                    run.plan->limits.max_output_bytes - d.text.size())
                    stop(run, d, ErrorEnvelope::make(ErrorCode::result_too_large));
                else if (!receipt_budget.reserve({.output_bytes = observation.segment.text.size()}))
                    stop(run, d, ErrorEnvelope::make(ErrorCode::budget_exceeded));
                else if (observation.segment.complete) {
                    auto valid = validate_value(JsonValue{d.text + observation.segment.text},
                                                d.invocation.output_schema);
                    if (!valid)
                        stop(run, d, valid.error());
                }
            }
        }
        if (observation.operation == NativeOperation::pause && d.proposal && !observation.error) {
            auto auth = authorize(run, d, AuthorizationBoundary::dispatch);
            auto pins = verify_plan_pins(*d.proposal->compiled.plan, config.capabilities);
            auto child_auth =
                gate.authorize_known_inputs(run.context, config.policy, *d.proposal->compiled.plan,
                                            config.capabilities, d.proposal->compiled.input_values,
                                            now_ms(), milliseconds(d.invocation.deadline.time()));
            if (accepting && auth && pins && run.controller->snapshot().child_creation_open &&
                child_auth &&
                d.proposal->dispatch_generation == observation.ticket.dispatch_generation) {
                auto budget = run.budget;
                auto charged = budget.reserve(
                    {.jobs = d.proposal->compiled.plan->static_jobs, .suspensions = 1});
                auto child_duration =
                    std::chrono::milliseconds(d.proposal->compiled.plan->limits.timeout_ms);
                if (d.proposal->compiled.plan->single_root_inference)
                    child_duration = std::min(
                        child_duration, std::chrono::milliseconds(
                                            d.proposal->compiled.plan->steps.front().timeout_ms));
                auto child_bound = Deadline::after(config.clock->now(), child_duration);
                if (!child_bound) {
                    stop(run, d, child_bound.error());
                    return;
                }
                const auto child_deadline = Deadline::at(
                    std::min(d.invocation.deadline.time(), child_bound.value().time()));
                const std::array specs{ChildSpec{child_deadline, true}};
                if (charged) {
                    auto spawned =
                        d.native->accept_with_children(*run.controller, observation, specs);
                    if (!spawned) {
                        d.retained_observation = std::move(observation);
                        return;
                    }
                    run.budget = std::move(budget);
                    d.retained_observation.reset();
                    d.native_quiescent = true;
                    const auto child = spawned.value().children.front();
                    auto compiled = std::move(d.proposal->compiled);
                    d.proposal.reset();
                    WorkflowInvocation invocation;
                    invocation.job = child;
                    invocation.deadline = child_deadline;
                    if (compiled.plan->single_root_inference) {
                        const auto &step = compiled.plan->steps.front();
                        invocation.node = step.id;
                        invocation.child_envelope = step.child_envelope;
                        invocation.capability_pin = step.capability_pin;
                        invocation.effects = step.effects;
                        invocation.output_schema = step.output_schema;
                        invocation.input = arguments(step, compiled.input_values);
                    } else {
                        invocation.coordinating = true;
                        invocation.input = JsonValue::Object{};
                    }
                    auto queued = run.controller->queue(child);
                    auto added = queued ? add_driver(run, std::move(invocation))
                                        : Result<void>::failure(queued.error());
                    if (!added) {
                        stop(run, d, added.error());
                        return;
                    }
                    auto &driver = run.drivers.at(child);
                    driver.dynamic_parent = d.invocation.job;
                    driver.execution_plan = compiled.plan;
                    driver.child_depth = d.child_depth + 1;
                    if (!compiled.plan->single_root_inference) {
                        auto machine = WorkflowMachine::create_at(child, compiled.plan,
                                                                  std::move(compiled.input_values),
                                                                  *run.controller, *config.clock);
                        if (!machine) {
                            stop(run, driver, machine.error());
                            stop(run, d, machine.error());
                            return;
                        }
                        driver.fragment = std::move(machine).value();
                    }
                    request_host_release(d);
                    return;
                }
            }
            d.proposal.reset();
            stop(run, d,
                 !auth ? auth.error()
                       : (!pins ? pins.error()
                                : (!child_auth ? child_auth.error()
                                               : ErrorEnvelope::make(ErrorCode::budget_exceeded))));
        }
        auto accepted = d.native->accept(*run.controller, observation);
        if (!accepted) {
            d.retained_observation = std::move(observation);
            return;
        }
        d.retained_observation.reset();
        run.budget = std::move(receipt_budget);
        if (observation.control_rejected) {
            if (observation.operation == NativeOperation::inject && d.injection)
                stop(run, d,
                     observation.error.value_or(ErrorEnvelope::make(ErrorCode::invalid_result)));
            return;
        }
        if (observation.operation == NativeOperation::inject && !observation.error)
            d.injection.reset();
        d.native_quiescent = observation.quiescent;
        if (observation.error)
            d.error = *observation.error;
        if (observation.operation == NativeOperation::load && !observation.error) {
            d.native_loaded = true;
            auto materialize = [&](std::size_t bytes, std::optional<AllocationIdentity> &target) {
                if (bytes == 0)
                    return;
                auto &prepared =
                    (&target == &d.model_allocation) ? d.prepared_model : d.prepared_state;
                if (!prepared) {
                    stop(run, d, ErrorEnvelope::make(ErrorCode::invariant_violation));
                    return;
                }
                AllocationIdentity identity = *prepared;
                ResourceVector footprint;
                footprint[ResourceKind::ram] = bytes;
                auto r = resources.materialized(*d.reservation, identity, footprint);
                if (!r) {
                    stop(run, d, r.error());
                    return;
                }
                target = identity;
            };
            if (d.residency_loading)
                materialize(observation.model_bytes, d.model_allocation);
            materialize(observation.state_bytes, d.state_allocation);
            if (d.model_allocation && d.state_allocation) {
                std::vector<AllocationIdentity> manifest{*d.state_allocation};
                if (d.residency_loading)
                    manifest.push_back(*d.model_allocation);
                auto complete = resources.materialization_complete(*d.reservation, manifest,
                                                                   config.clock->now());
                if (!complete)
                    stop(run, d, complete.error());
                else if (d.residency_loading && d.residency_key) {
                    auto resident = residency.completed(*d.residency_key, d.reservation->owner,
                                                        std::move(observation.resident_model),
                                                        *d.model_allocation, config.clock->now());
                    if (!resident)
                        stop(run, d, resident.error());
                    else {
                        d.residency_loading = false;
                        d.residency_attached = true;
                    }
                }
            }
            observation.resident_model.reset();
        }
        const auto accepted_state = run.controller->job(d.invocation.job);
        if (observation.operation == NativeOperation::step && accepted_state &&
            (accepted_state.value().state == JobState::running ||
             accepted_state.value().terminal_intent == JobState::succeeded)) {
            if (observation.segment.text.size() > run.plan->limits.max_output_bytes - d.text.size())
                stop(run, d, ErrorEnvelope::make(ErrorCode::result_too_large));
            else
                d.text += observation.segment.text;
            if (observation.segment.complete && !d.error)
                d.output = JsonValue{d.text};
        }
        if (observation.operation == NativeOperation::pause)
            request_host_release(d);
        if (observation.operation == NativeOperation::release && !observation.error) {
            d.native_released = true;
            auto release_allocation = [&](std::optional<AllocationIdentity> &identity,
                                          bool proven) {
                if (!identity || !proven)
                    return;
                auto op = operations.next();
                if (!op)
                    return;
                (void)resources.drop_reference(*identity, d.invocation.job);
                auto requested = resources.request_release(*identity, OperationId{op.value()});
                if (requested) {
                    auto r = resources.release_confirmed({*identity, OperationId{op.value()}, true},
                                                         config.clock->now());
                    if (r && r.value())
                        identity.reset();
                }
            };
            release_allocation(d.state_allocation,
                               observation.release.quiescent &&
                                   observation.release.released_state_bytes > 0);
            if (d.residency_key && d.residency_attached && d.model_allocation) {
                auto operation = operations.next();
                if (operation) {
                    auto released =
                        residency.release_job(*d.residency_key, d.invocation.job,
                                              OperationId{operation.value()}, config.clock->now());
                    if (released &&
                        (released.value() ||
                         residency.availability(*d.residency_key, config.clock->now()) ==
                             ModelLoadDecision::resident_available)) {
                        d.model_allocation.reset();
                        d.residency_attached = false;
                    }
                }
            } else
                release_allocation(d.model_allocation, observation.model_allocation_released);
            if (d.residency_loading && d.residency_key && d.reservation) {
                (void)residency.cancel_loading(*d.residency_key, d.reservation->owner);
                (void)residency.loading_stopped(*d.residency_key, d.reservation->owner,
                                                ContainmentProof::worker_quiesced);
                d.residency_loading = false;
            }
            request_host_release(d);
        }
    }
    void consume(Run &run, Driver &d) {
        if (d.closing_native.valid() &&
            d.closing_native.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            d.native = d.closing_native.get();
        if (d.preparing_native.valid() &&
            d.preparing_native.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto prepared = d.preparing_native.get();
            if (!prepared) {
                stop(run, d, prepared.error());
            } else
                d.native = std::make_unique<InferenceMachine>(std::move(prepared).value(),
                                                              d.invocation.job.value());
        }
        if (d.native) {
            if (d.retained_observation)
                accept_native(run, d, *d.retained_observation);
            else if (auto observation = d.native->take())
                accept_native(run, d, std::move(*observation));
        }
        if (d.provider.valid() &&
            d.provider.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            auto outcome = d.provider.get();
            if (!accepting)
                stop(run, d, ErrorEnvelope::make(ErrorCode::run_cancelled));
            d.outcome = outcome.external_outcome;
            d.native_quiescent = true;
            if (outcome.error && outcome.retry_evidence) {
                auto evidence = *outcome.retry_evidence;
                evidence.max_attempts = d.invocation.max_attempts;
                auto state = run.controller->job(d.invocation.job);
                evidence.job_stopping_or_terminal =
                    !state || state.value().state != JobState::running;
                auto retry =
                    authorize_retry(config.capabilities.capabilities.at(d.registration->capability),
                                    evidence, now_ms(), milliseconds(d.invocation.deadline.time()));
                auto current = authorize(run, d, AuthorizationBoundary::retry);
                auto deadline = Deadline::after(config.clock->now(),
                                                std::chrono::milliseconds(d.invocation.backoff_ms));
                if (retry && current && deadline &&
                    deadline.value().time() < d.invocation.deadline.time()) {
                    d.retry_evidence = std::move(evidence);
                    d.retry_error = *outcome.error;
                    d.retry_not_before = deadline.value().time();
                    d.retry_waiting = true;
                    request_host_release(d);
                } else {
                    stop(run, d, current ? *outcome.error : current.error());
                    (void)run.controller->observe_stopped(*d.ticket, {true, false, d.outcome});
                    request_host_release(d);
                }
            } else if (outcome.error) {
                stop(run, d, *outcome.error);
                (void)run.controller->observe_stopped(*d.ticket, {true, false, d.outcome});
            } else {
                auto encoded = outcome.value ? canonical_json(*outcome.value)
                                             : failure<std::string>(ErrorCode::invalid_result);
                auto handles = outcome.value ? gate.validate_result_handles(
                                                   run.context, config.policy, *d.execution_plan,
                                                   config.capabilities, d.registration->capability,
                                                   *outcome.value, now_ms())
                                             : failure<void>(ErrorCode::invalid_result);
                auto charged = encoded
                                   ? run.budget.reserve({.output_bytes = encoded.value().size()})
                                   : Result<void>::failure(encoded.error());
                if (!charged || !handles) {
                    stop(run, d, !handles ? handles.error() : charged.error());
                    (void)run.controller->observe_stopped(*d.ticket, {true, false, d.outcome});
                    request_host_release(d);
                    return;
                }
                d.output = std::move(outcome.value);
                d.retry_evidence.reset();
                d.retry_error.reset();
                auto state = run.controller->job(d.invocation.job);
                if (state && state.value().state == JobState::cancelling)
                    (void)run.controller->observe_stopped(*d.ticket, {true, false, d.outcome});
                else
                    (void)run.controller->complete(*d.ticket, d.outcome);
            }
            request_host_release(d);
        }
        if (d.residency_key && d.native_released && d.model_allocation) {
            auto settled = residency.reconcile(*d.residency_key, config.clock->now());
            if (settled && settled.value()) {
                d.model_allocation.reset();
                d.residency_attached = false;
            }
        }
        release_ledger(d);
        if (d.native && d.native_released && !d.native->pending() && !d.reservation &&
            !d.state_allocation && !d.model_allocation && !d.closing_native.valid() &&
            d.close_attempts < 4) {
            ++d.close_attempts;
            auto native = std::move(d.native);
            d.closing_native = std::async(
                std::launch::async,
                [native = std::move(native)]() mutable -> std::unique_ptr<InferenceMachine> {
                    if (native->close())
                        return {};
                    return std::move(native);
                });
        }
        if (d.retry_waiting && !d.reservation && !d.lease) {
            auto current = authorize(run, d, AuthorizationBoundary::retry);
            auto state = run.controller->job(d.invocation.job);
            if (!current || !state || state.value().state != JobState::running) {
                stop(run, d, current ? d.retry_error.value() : current.error());
                if (d.ticket)
                    (void)run.controller->observe_stopped(*d.ticket, {true, false, d.outcome});
                d.retry_waiting = false;
            } else {
                auto charge = run.budget.reserve({.attempts = 1});
                auto retry = charge ? run.controller->retry(*d.ticket, *d.retry_not_before, true,
                                                            true, true, d.outcome)
                                    : Result<void>::failure(charge.error());
                if (!retry) {
                    stop(run, d, retry.error());
                    (void)run.controller->observe_stopped(*d.ticket, {true, false, d.outcome});
                }
                d.retry_waiting = false;
            }
        }
    }
    void terminal(Run &run, Driver &d) {
        auto job = run.controller->job(d.invocation.job);
        if (!job)
            return;
        if (job.value().state == JobState::finalizing && d.reservation && !d.native &&
            !d.provider.valid() && !d.preparing_native.valid()) {
            d.native_quiescent = true;
            if (!d.cleanup_started)
                d.cleanup_started = config.clock->now();
            request_host_release(d);
            release_ledger(d);
            if (d.reservation && !d.cleanup_transferred &&
                config.clock->now() - *d.cleanup_started >= std::chrono::seconds(5)) {
                auto transfer = resources.transfer_acquisition_to_cleanup(
                    *d.reservation, ContainmentProof::worker_quiesced,
                    config.clock->now() + std::chrono::seconds(5), config.clock->now());
                if (transfer) {
                    d.cleanup_transferred = true;
                    (void)run.controller->acknowledge_cleanup(d.invocation.job,
                                                              {false, true, true, true});
                    (void)run.controller->finalize(d.invocation.job);
                }
            }
        }
        if (job.value().state == JobState::finalizing && !d.lease && !d.reservation &&
            !d.state_allocation && !d.model_allocation && (!d.native || d.native_released) &&
            !d.provider.valid() && !d.preparing_native.valid()) {
            auto ack =
                run.controller->acknowledge_cleanup(d.invocation.job, {true, false, false, false});
            if (ack)
                (void)run.controller->finalize(d.invocation.job);
            job = run.controller->job(d.invocation.job);
        }
        if (!job || !done(job.value().state) || d.published)
            return;
        if (d.dynamic_parent) {
            auto parent = run.drivers.find(*d.dynamic_parent);
            if (parent != run.drivers.end()) {
                if (job.value().state == JobState::succeeded && d.output) {
                    auto value = canonical_json(*d.output);
                    if (value && value.value().size() <= 255) {
                        auto operation = operations.next();
                        if (operation) {
                            parent->second.injection = std::move(value).value();
                            parent->second.injection_id = operation.value();
                            d.published = true;
                            auto parent_state = run.controller->job(*d.dynamic_parent);
                            if (parent_state)
                                (void)run.controller->wake(
                                    *d.dynamic_parent, parent_state.value().suspension_generation);
                        }
                    } else
                        stop(run, parent->second, ErrorEnvelope::make(ErrorCode::result_too_large));
                } else
                    stop(run, parent->second,
                         d.error.value_or(ErrorEnvelope::make(
                             job.value().error.value_or(ErrorCode::upstream_failure))));
            }
            return;
        }
        if (d.invocation.job == run.controller->root()) {
            d.published = true;
            run.result.pending = false;
            run.terminal_since = config.clock->now();
            run.result.external_outcome = d.outcome;
            if (job.value().state == JobState::succeeded)
                run.result.value = d.output;
            else
                run.result.error = d.error.value_or(
                    ErrorEnvelope::make(job.value().error.value_or(ErrorCode::internal_error)));
        } else if (d.owner_machine) {
            if (job.value().state == JobState::succeeded && d.output) {
                auto delivered = d.owner_machine->accept_result(d.invocation.job, *d.output);
                if (delivered)
                    d.published = true;
            } else {
                auto delivered = d.owner_machine->accept_failure(
                    d.invocation.job, d.error.value_or(ErrorEnvelope::make(
                                          job.value().error.value_or(ErrorCode::internal_error))));
                if (delivered)
                    d.published = true;
            }
        }
    }
    void native_step(Run &run, Driver &d, const JobSnapshot &state) {
        if (!d.native || d.native->pending() || !d.ticket)
            return;
        if (!accepting && state.state == JobState::running) {
            stop(run, d, ErrorEnvelope::make(ErrorCode::run_cancelled));
            return;
        }
        NativeOperation operation;
        DispatchChecks checks{};
        NativeCommand payload;
        if (state.state == JobState::cancelling)
            operation = NativeOperation::stop;
        else if (state.state == JobState::finalizing && !d.native_released)
            operation = NativeOperation::release;
        else if (state.state != JobState::running)
            return;
        else {
            auto auth = authorize(run, d,
                                  d.resuming ? AuthorizationBoundary::resume
                                             : AuthorizationBoundary::dispatch);
            if (!auth) {
                stop(run, d, auth.error());
                return;
            }
            if (!d.lease ||
                !(d.native_loaded ? resources.ready_for_use(*d.lease, config.clock->now())
                                  : resources.lease_current(*d.lease, config.clock->now()))) {
                stop(run, d, ErrorEnvelope::make(ErrorCode::resource_unavailable));
                return;
            }
            checks = {true, true, true, true};
            operation = !d.native_loaded
                            ? NativeOperation::load
                            : ((state.pause_requested || d.proposal)
                                   ? NativeOperation::pause
                                   : (d.resuming ? NativeOperation::resume
                                                 : (d.injection ? NativeOperation::inject
                                                                : NativeOperation::step)));
            if (operation == NativeOperation::inject && d.injection) {
                payload.input = *d.injection;
                payload.injection_command = d.injection_id;
            }
            if (d.resuming && d.native->native_state()) {
                payload.resume_pins = d.native->native_state()->pins;
                payload.suspension_generation = d.native->native_state()->suspension_generation;
            }
        }
        if (operation == NativeOperation::pause && !d.proposal) {
            auto charge = run.budget.reserve({.suspensions = 1});
            if (!charge) {
                stop(run, d, charge.error());
                return;
            }
        }
        auto submitted =
            d.native->submit(*run.controller, *d.ticket, operation, checks, std::move(payload));
        if (!submitted) {
            stop(run, d, submitted.error());
            return;
        }
        d.native_quiescent = false;
        if (operation == NativeOperation::resume)
            d.resuming = false;
    }
    void dispatch(Run &run, Driver &d) {
        auto job = run.controller->job(d.invocation.job);
        if (!job || job.value().state != JobState::queued)
            return;
        if (d.invocation.coordinating) {
            if (!config.policy.enabled || run.context.revoked ||
                now_ms() >= run.context.expires_at_ms) {
                stop(run, d, ErrorEnvelope::make(ErrorCode::permission_denied));
                return;
            }
            auto charged = run.budget.reserve({.control_steps = 1});
            if (!charged) {
                stop(run, d, charged.error());
                return;
            }
            auto ticket = run.controller->dispatch(d.invocation.job, {true, true, true, true});
            if (!ticket)
                return;
            d.ticket = ticket.value();
            scheduler.remove(d.invocation.job);
            auto *machine = d.fragment ? d.fragment.get() : d.owner_machine;
            if (!machine) {
                stop(run, d, ErrorEnvelope::make(ErrorCode::invariant_violation));
                return;
            }
            auto advanced = machine->advance(ticket.value());
            if (!advanced) {
                stop(run, d, advanced.error());
                return;
            }
            for (auto &invocation : advanced.value().invocations) {
                auto child_id = invocation.job;
                auto added = add_driver(run, std::move(invocation), machine);
                if (added) {
                    run.drivers.at(child_id).execution_plan = d.execution_plan;
                    run.drivers.at(child_id).child_depth = d.child_depth;
                }
                if (!added) {
                    stop(run, d, added.error());
                    return;
                }
            }
            if (advanced.value().completed_output) {
                d.output = std::move(advanced.value().completed_output);
                d.native_quiescent = true;
                (void)run.controller->complete(ticket.value());
            }
            return;
        }
        if (d.release_requested)
            return;
        if (workers() >= config.max_workers && !d.lease)
            return;
        if (d.registration->native && !d.native) {
            std::size_t retained = 0;
            for (const auto &[id, active_run] : runs) {
                (void)id;
                for (const auto &[active_job_id, driver] : active_run->drivers) {
                    (void)active_job_id;
                    if (driver.native || driver.preparing_native.valid() ||
                        driver.closing_native.valid())
                        ++retained;
                }
            }
            if (retained >= config.max_native_workers)
                return;
        }
        std::size_t active = 0;
        for (const auto &[other_id, other] : run.drivers) {
            (void)other_id;
            if (!other.invocation.coordinating &&
                (other.lease || other.provider.valid() || other.preparing_native.valid()))
                ++active;
        }
        if (active >= run.plan->limits.max_parallelism && !d.lease)
            return;
        auto authorized = authorize(run, d,
                                    job.value().pending_resume
                                        ? AuthorizationBoundary::resume
                                        : (d.retry_evidence ? AuthorizationBoundary::retry
                                                            : AuthorizationBoundary::dispatch));
        if (!authorized) {
            stop(run, d, authorized.error());
            return;
        }
        if (!d.reservation) {
            auto env = envelopes.find(d.registration->resource);
            if (env == envelopes.end())
                return;
            if (d.registration->native && !d.native_loaded) {
                if (!d.residency_key)
                    d.residency_key =
                        model_key(*d.registration,
                                  config.capabilities.capabilities.at(d.registration->capability),
                                  env->second.epoch);
                if (residency.availability(*d.residency_key, config.clock->now()) ==
                    ModelLoadDecision::wait_without_lease)
                    return;
            }
            auto operation = operations.next();
            if (!operation) {
                stop(run, d, operation.error());
                return;
            }
            ResourceRequest request;
            request.operation = OperationId{operation.value()};
            request.owner = {d.invocation.job,
                             AttemptId{job.value().attempt ? job.value().attempt : 1},
                             DispatchGeneration{job.value().dispatch_generation + 1}};
            request.resource = d.registration->resource;
            request.host_epoch = env->second.epoch;
            request.worker = d.registration->worker_generation;
            request.incremental = d.registration->requirements;
            request.run_limit = d.registration->run_resource_limit;
            request.deadline = d.invocation.deadline.time();
            request.acquisition_deadline = d.acquisition_deadline;
            if (d.residency_key && !d.native_loaded &&
                residency.availability(*d.residency_key, config.clock->now()) ==
                    ModelLoadDecision::resident_available) {
                auto resident = residency.view(*d.residency_key, config.clock->now());
                if (!resident) {
                    stop(run, d, resident.error());
                    return;
                }
                request.incremental[ResourceKind::ram] -= resident.value().model_bytes;
                request.shared.push_back(resident.value().allocation);
                d.model_allocation = resident.value().allocation;
            }
            if (d.native_loaded) {
                for (const auto &allocation : {d.state_allocation, d.model_allocation})
                    if (allocation) {
                        auto footprint = resources.allocation(*allocation);
                        if (!footprint) {
                            stop(run, d, ErrorEnvelope::make(ErrorCode::state_unavailable));
                            return;
                        }
                        for (std::size_t index = 0; index < request.incremental.values.size();
                             ++index) {
                            const auto retained = footprint->footprint.values[index];
                            if (retained > request.incremental.values[index]) {
                                stop(run, d, ErrorEnvelope::make(ErrorCode::resource_unavailable));
                                return;
                            }
                            request.incremental.values[index] -= retained;
                        }
                    }
                if (d.state_allocation)
                    request.shared.push_back(*d.state_allocation);
                if (d.model_allocation)
                    request.shared.push_back(*d.model_allocation);
            }
            auto charged = run.budget.reserve({.resource_operations = 1});
            if (!charged) {
                stop(run, d, charged.error());
                return;
            }
            auto reservation = resources.reserve(request, config.clock->now());
            if (!reservation) {
                d.reservation = resources.ticket_for(request.operation);
                if (d.reservation)
                    stop(run, d, reservation.error());
                return;
            }
            d.reservation = reservation.value();
        }
        // Budget eligibility is checked before the resource ledger can gain an active lease.
        // The actor is serialized, so no competing dispatch can consume this budget in between.
        auto eligible = run.controller->dispatch_eligible(d.invocation.job);
        if (!eligible) {
            if (eligible.error().code() != ErrorCode::resource_unavailable)
                stop(run, d, eligible.error());
            return;
        }
        auto next_budget = run.budget;
        auto charged = next_budget.reserve({.attempts = job.value().attempt ? 0ULL : 1ULL});
        if (!charged) {
            stop(run, d, charged.error());
            return;
        }
        const auto now = config.clock->now();
        auto grant_ready =
            resources.dispatch_eligible(*d.reservation, {true, true, true, true}, now);
        if (!grant_ready)
            return;
        auto ticket = run.controller->dispatch(d.invocation.job, {true, true, true, true});
        if (!ticket) {
            stop(run, d, ticket.error());
            return;
        }
        d.ticket = ticket.value();
        // No callback or competing ledger mutation occurs between the validated grant and
        // commit. Event preparation may fail before a lease is made active.
        auto lease = resources.commit_dispatch(*d.reservation, {true, true, true, true}, now);
        if (!lease) {
            stop(run, d, lease.error());
            d.native_quiescent = true;
            request_host_release(d);
            return;
        }
        d.lease = lease.value();
        run.budget = std::move(next_budget);
        if (d.native_loaded) {
            auto manifest = resources.materialization_complete(*d.reservation, {}, now);
            if (!manifest) {
                stop(run, d, manifest.error());
                return;
            }
        }
        d.resuming = job.value().pending_resume;
        scheduler.remove(d.invocation.job);
        if (d.registration->native && !d.native) {
            auto config_copy = *d.registration->native;
            if (d.model_allocation && d.residency_key) {
                auto holder =
                    residency.attach(*d.residency_key, d.invocation.job,
                                     d.registration->run_resource_limit, config.clock->now());
                if (!holder) {
                    stop(run, d, holder.error());
                    return;
                }
                config_copy.shared_model = std::move(holder).value();
                d.residency_attached = true;
            } else if (d.residency_key) {
                auto begun =
                    residency.begin(*d.residency_key, d.reservation->owner, *d.lease, false,
                                    d.invocation.deadline.time(), config.clock->now());
                if (!begun) {
                    stop(run, d, begun.error());
                    d.native_quiescent = true;
                    request_host_release(d);
                    return;
                }
                d.residency_loading = true;
            }
            auto model_id = d.model_allocation
                                ? Result<AllocationIdentity>::success(*d.model_allocation)
                                : resources.next_allocation_identity(*d.reservation);
            auto state_id = resources.next_allocation_identity(*d.reservation);
            if (!model_id || !state_id) {
                stop(run, d, ErrorEnvelope::make(ErrorCode::resource_unavailable));
                d.native_quiescent = true;
                request_host_release(d);
                return;
            }
            d.prepared_model = model_id.value();
            d.prepared_state = state_id.value();
            const auto &inputs = std::get<JsonValue::Object>(d.invocation.input.data);
            auto prompt = inputs.find(d.registration->prompt_field);
            if (prompt == inputs.end() ||
                !std::holds_alternative<std::string>(prompt->second.data)) {
                stop(run, d, ErrorEnvelope::make(ErrorCode::invalid_request));
                d.native_quiescent = true;
                request_host_release(d);
                return;
            }
            config_copy.prompt = std::get<std::string>(prompt->second.data);
            auto factory = config.native_worker_factory;
            d.preparing_native =
                std::async(std::launch::async, [factory = std::move(factory),
                                                config_copy = std::move(config_copy)]() mutable {
                    try {
                        return factory(std::move(config_copy));
                    } catch (...) {
                        return Result<std::unique_ptr<NativeWorkerPort>>::failure(
                            ErrorEnvelope::make(ErrorCode::native_execution_failure));
                    }
                });
        } else if (d.registration->provider) {
            AuthorizationRequest request;
            request.capability = d.invocation.capability_pin->identifier;
            request.boundary =
                d.retry_evidence ? AuthorizationBoundary::retry : AuthorizationBoundary::dispatch;
            request.now_ms = now_ms();
            request.deadline_ms = milliseconds(d.invocation.deadline.time());
            request.concrete_inputs = std::get<JsonValue::Object>(d.invocation.input.data);
            request.concrete_input_digest = authorized.value().concrete_input_digest;
            auto grant = adapter_authority.authorize(
                run.context, config.policy, *d.execution_plan, config.capabilities, request,
                *d.ticket, *d.invocation.capability_pin, d.invocation.input,
                std::to_string(config.instance.high()) + ":" +
                    std::to_string(config.instance.low()) + ":" +
                    std::to_string(run.controller->id().value()) + ":" +
                    std::to_string(d.invocation.job.value()),
                AdapterPurpose::invoke, d.retry_evidence ? &*d.retry_evidence : nullptr);
            if (!grant) {
                stop(run, d, grant.error());
                d.native_quiescent = true;
                request_host_release(d);
                return;
            }
            auto contract = config.capabilities.capabilities.at(request.capability);
            AdapterRequest input{std::move(grant).value(), d.invocation.input,
                                 static_cast<std::size_t>(contract.max_output_bytes)};
            auto binding = adapters.at(request.capability);
            auto clock = config.clock;
            d.provider = std::async(std::launch::async, [binding = std::move(binding), clock,
                                                         input = std::move(input)]() mutable {
                return binding->invoke(std::move(input), *clock);
            });
        }
    }
    void progress() {
        submissions.expire(now_ms());
        for (auto it = pending.begin(); it != pending.end();) {
            auto decision = submissions.poll(it->second.ticket);
            if (decision && decision.value() && decision.value()->rejection) {
                it->second.promise->set_value(
                    Result<SubmissionReceipt>::failure(*decision.value()->rejection));
                (void)admission.settle_claim(it->second.admission_claim);
                it = pending.erase(it);
            } else
                ++it;
        }
        resources.close_observations(config.clock->now());
        for (auto &[id, runptr] : runs) {
            (void)id;
            auto &run = *runptr;
            (void)run.controller->check_deadlines();
            for (auto &[job, d] : run.drivers) {
                (void)job;
                consume(run, d);
            }
            for (auto &[job, d] : run.drivers) {
                auto state = run.controller->job(job);
                if (!state)
                    continue;
                if (state.value().state == JobState::queued)
                    enqueue(run, job, d);
                native_step(run, d, state.value());
                terminal(run, d);
            }
        }
        for (auto it = runs.begin(); it != runs.end();) {
            auto &run = *it->second;
            if (run.terminal_since && milliseconds(config.clock->now() - *run.terminal_since) >=
                                          config.submissions.retention_ms) {
                auto &store = run.controller->observation_store();
                if (store.close() && store.expire()) {
                    // Terminal drivers have no active operation; their worker destructors
                    // still belong off the actor, even when only an idle thread remains.
                    bool no_native = true;
                    for (const auto &[job, driver] : run.drivers) {
                        (void)job;
                        if (driver.native || driver.closing_native.valid())
                            no_native = false;
                    }
                    if (no_native) {
                        it = runs.erase(it);
                        continue;
                    }
                }
            }
            ++it;
        }
        std::uint64_t floor = run_retirement_floor;
        for (const auto &[id, run] : runs) {
            (void)run;
            floor = std::min(floor, id.value());
        }
        for (const auto &[id, claim] : pending) {
            (void)id;
            floor = std::min(floor, claim.ticket.run.value());
        }
        for (auto &[cap, binding] : adapters) {
            (void)cap;
            (void)binding->try_retire_before(config.instance, floor);
        }
        (void)residency.retire_settled();
        (void)resources.retire_settled();
        if (!accepting) {
            auto drain = drain_snapshot();
            cleanup_settled.store(pending.empty() && drain.active_workers == 0 &&
                                      drain.nonterminal_jobs == 0 &&
                                      drain.unresolved_resources == 0,
                                  std::memory_order_release);
            return;
        }
        std::vector<RunController *> controllers;
        controllers.reserve(runs.size());
        for (auto &[id, run] : runs) {
            (void)id;
            controllers.push_back(run->controller.get());
        }
        auto choice = scheduler.select(controllers, config.clock->now());
        if (choice && choice.value()) {
            auto run = runs.find(choice.value()->job.run());
            if (run != runs.end()) {
                auto d = run->second->drivers.find(choice.value()->job);
                if (d != run->second->drivers.end()) {
                    if (!d->second.acquisition_deadline)
                        d->second.acquisition_deadline =
                            choice.value()->acquisition_deadline.time();
                    if (d->second.acquisition_deadline &&
                        config.clock->now() >= *d->second.acquisition_deadline && !d->second.lease)
                        stop(*run->second, d->second,
                             ErrorEnvelope::make(
                                 ErrorCode::resource_unavailable, ErrorStage::admission,
                                 ExternalOutcome::not_dispatched, RetryDisposition::prohibited,
                                 ErrorReason::resource_deadlock));
                    else
                        dispatch(*run->second, d->second);
                }
            }
        }
    }
    void loop() {
        for (;;) {
            std::function<void()> action;
            {
                std::unique_lock lock(mutex);
                condition.wait_for(lock, std::chrono::milliseconds(1), [&] {
                    return stopping || !mandatory.empty() || !normal.empty();
                });
                if (stopping && mandatory.empty() && normal.empty())
                    break;
                auto &queue = !mandatory.empty() ? mandatory : normal;
                if (!queue.empty()) {
                    action = std::move(queue.front());
                    queue.pop_front();
                    condition.notify_all();
                }
            }
            if (action)
                action();
            try {
                progress();
            } catch (...) {
                accepting = false;
                for (auto &[id, run] : runs) {
                    (void)id;
                    (void)run->controller->cancel_run(ErrorCode::internal_error);
                }
            }
        }
    }
};

RuntimeInstance::RuntimeInstance(std::shared_ptr<Impl> impl, RuntimeRetentionSlot slot)
    : impl_(std::move(impl)), retention_slot_(std::move(slot)) {
    impl_->actor = std::thread([state = impl_.get()] { state->loop(); });
}
Result<std::unique_ptr<RuntimeInstance>> RuntimeInstance::create(RuntimeConfiguration config) {
    try {
        if (config.drain_timeout <= Duration::zero() ||
            config.cleanup_timeout <= Duration::zero() ||
            config.drain_timeout > std::chrono::hours(1) ||
            config.cleanup_timeout > std::chrono::hours(1))
            return failure<std::unique_ptr<RuntimeInstance>>(ErrorCode::invalid_request);
        auto slot = RuntimeSupervisor::process().reserve();
        if (!slot)
            return Result<std::unique_ptr<RuntimeInstance>>::failure(slot.error());
        if (!config.max_runs || !config.normal_commands || !config.mandatory_commands ||
            !config.max_workers || !config.max_native_workers || config.max_native_workers > 4096 ||
            config.max_runs > 1024 || config.registrations.size() > 256)
            return failure<std::unique_ptr<RuntimeInstance>>(ErrorCode::invalid_request);
        if (!config.instance.valid()) {
            std::random_device random;
            config.instance = {((std::uint64_t{random()} << 32) | random()),
                               ((std::uint64_t{random()} << 32) | random())};
        }
        if (!config.clock)
            config.clock = std::make_shared<SteadyMonotonicClock>();
        if (!config.native_worker_factory)
            config.native_worker_factory =
                [](NativeWorkerConfig value) -> Result<std::unique_ptr<NativeWorkerPort>> {
                auto worker = ThreadNativeWorker::create(std::move(value));
                if (!worker)
                    return Result<std::unique_ptr<NativeWorkerPort>>::failure(worker.error());
                return Result<std::unique_ptr<NativeWorkerPort>>::success(
                    std::move(worker).value());
            };
        if (!config.host)
            config.host = std::make_shared<UnavailableHostAuthority>();
        for (const auto &r : config.registrations) {
            if (!config.capabilities.capabilities.contains(r.capability) ||
                (!r.native && !r.provider) || (r.native && r.provider) || r.requirements.empty() ||
                r.run_resource_limit.empty())
                return failure<std::unique_ptr<RuntimeInstance>>(ErrorCode::invalid_request);
        }
        auto bound = bind_registration_contracts(config.capabilities, config.registrations);
        if (!bound)
            return Result<std::unique_ptr<RuntimeInstance>>::failure(bound.error());
        auto instance = std::unique_ptr<RuntimeInstance>(new RuntimeInstance(
            std::make_shared<Impl>(std::move(config)), std::move(slot).value()));
        return Result<std::unique_ptr<RuntimeInstance>>::success(std::move(instance));
    } catch (...) {
        return failure<std::unique_ptr<RuntimeInstance>>(ErrorCode::resource_unavailable);
    }
}
RuntimeInstance::~RuntimeInstance() {
    (void)shutdown();
    auto owner = std::move(impl_);
    if (owner->cleanup_complete())
        owner->close_and_join();
    else if (!retention_slot_.retain(owner))
        std::terminate();
}
RuntimeInstanceId RuntimeInstance::instance_id() const noexcept { return impl_->config.instance; }
Result<RuntimeCompilationSnapshot> RuntimeInstance::compilation_snapshot() {
    auto state = impl_;
    return state->call<RuntimeCompilationSnapshot>([state]() {
        return Result<RuntimeCompilationSnapshot>::success(
            {state->config.instance, state->config.capabilities, state->config.compiler});
    });
}
Result<SubmissionReceipt> RuntimeInstance::submit(const AuthorizationContext &context,
                                                  std::string_view json) {
    return submit_impl(context, json, std::nullopt);
}
Result<SubmissionReceipt> RuntimeInstance::submit_pinned(const AuthorizationContext &context,
                                                         std::string_view json,
                                                         PinnedSubmissionIdentity expected) {
    return submit_impl(context, json, std::move(expected));
}
Result<SubmissionReceipt>
RuntimeInstance::submit_impl(const AuthorizationContext &context, std::string_view json,
                             std::optional<PinnedSubmissionIdentity> expected) {
    auto state = impl_;
    if (json.size() > 1048576)
        return failure<SubmissionReceipt>(ErrorCode::invalid_request);
    auto identity = validate_submission_identity(json);
    if (!identity)
        return Result<SubmissionReceipt>::failure(identity.error());
    if (expected &&
        (expected->instance != instance_id() ||
         expected->request_digest != identity.value().request_digest ||
         expected->plan_fingerprint.size() != 64 ||
         expected->plan_fingerprint.find_first_not_of("0123456789abcdef") != std::string::npos))
        return failure<SubmissionReceipt>(ErrorCode::plan_stale);
    struct Preparation {
        SubmissionTicket ticket;
        std::shared_future<Result<SubmissionReceipt>> decision;
        CapabilitySnapshot registry;
    };
    auto preparation = state->call<Preparation>(
        [state, context, identity = identity.value()]() mutable -> Result<Preparation> {
            if (!state->accepting || context.subject.empty() || context.subject.size() > 128 ||
                context.revoked || state->now_ms() >= context.expires_at_ms ||
                !state->config.policy.enabled)
                return failure<Preparation>(ErrorCode::permission_denied);
            auto ticket = state->submissions.claim(context.subject, identity.request_kind,
                                                   identity.idempotency_key,
                                                   identity.request_digest, state->now_ms());
            if (!ticket)
                return Result<Preparation>::failure(ticket.error());
            if (!ticket.value().owner) {
                auto existing = state->submissions.poll(ticket.value());
                if (existing && existing.value() && existing.value()->run) {
                    auto promise = std::make_shared<std::promise<Result<SubmissionReceipt>>>();
                    auto future = promise->get_future().share();
                    promise->set_value(
                        Result<SubmissionReceipt>::success({*existing.value()->run, true}));
                    state->submissions.release_receipt(ticket.value());
                    return Result<Preparation>::success({ticket.value(), future, {}});
                }
                if (existing && existing.value() && existing.value()->rejection) {
                    auto promise = std::make_shared<std::promise<Result<SubmissionReceipt>>>();
                    auto future = promise->get_future().share();
                    promise->set_value(
                        Result<SubmissionReceipt>::failure(*existing.value()->rejection));
                    state->submissions.release_receipt(ticket.value());
                    return Result<Preparation>::success({ticket.value(), future, {}});
                }
                auto pending = state->pending.find(ticket.value().pending);
                if (pending == state->pending.end())
                    return failure<Preparation>(ErrorCode::state_unavailable);
                return Result<Preparation>::success({ticket.value(), pending->second.future, {}});
            }
            if (ticket.value().run.value() < std::numeric_limits<std::uint64_t>::max())
                state->run_retirement_floor =
                    std::max(state->run_retirement_floor, ticket.value().run.value() + 1);
            auto admission_claim = state->admission.claim();
            if (!admission_claim) {
                (void)state->submissions.reject(ticket.value(), admission_claim.error());
                return Result<Preparation>::failure(admission_claim.error());
            }
            auto promise = std::make_shared<std::promise<Result<SubmissionReceipt>>>();
            auto future = promise->get_future().share();
            state->pending.emplace(
                ticket.value().pending,
                Impl::ClaimWait{ticket.value(), promise, future, admission_claim.value()});
            return Result<Preparation>::success(
                {ticket.value(), future, state->config.capabilities});
        });
    if (!preparation)
        return Result<SubmissionReceipt>::failure(preparation.error());
    if (state->config.submission_preparation_hook) {
        try {
            state->config.submission_preparation_hook(preparation.value().ticket.pending,
                                                      preparation.value().ticket.owner);
        } catch (...) {
            if (preparation.value().ticket.owner)
                (void)state->call<void>(
                    [state, ticket = preparation.value().ticket] {
                        auto pending = state->pending.find(ticket.pending);
                        if (pending != state->pending.end()) {
                            auto error = ErrorEnvelope::make(ErrorCode::internal_error);
                            (void)state->submissions.reject(ticket, error);
                            pending->second.promise->set_value(
                                Result<SubmissionReceipt>::failure(error));
                            (void)state->admission.settle_claim(pending->second.admission_claim);
                            state->pending.erase(pending);
                        }
                        return Result<void>::success();
                    },
                    true);
            auto resolved = preparation.value().ticket.owner
                                ? preparation.value().decision.get()
                                : Result<SubmissionReceipt>::failure(
                                      ErrorEnvelope::make(ErrorCode::internal_error));
            (void)state->call<void>(
                [state, ticket = preparation.value().ticket] {
                    state->submissions.release_receipt(ticket);
                    return Result<void>::success();
                },
                true);
            return resolved;
        }
    }
    if (preparation.value().ticket.owner) {
        auto compiled = state->compiler.compile_submission(json, preparation.value().registry);
        auto admission = state->call<void>(
            [state, context, expected, ticket = preparation.value().ticket,
             compiled = std::move(compiled)]() mutable -> Result<void> {
                auto pending = state->pending.find(ticket.pending);
                if (pending == state->pending.end())
                    return failure<void>(ErrorCode::state_unavailable);
                Impl::PendingFailureGuard resolution{*state, ticket};
                auto reject = [&](ErrorEnvelope error) {
                    (void)state->submissions.reject(ticket, error);
                    pending->second.promise->set_value(Result<SubmissionReceipt>::failure(error));
                    (void)state->admission.settle_claim(pending->second.admission_claim);
                    state->pending.erase(pending);
                    return Result<void>::success();
                };
                if (!compiled)
                    return reject(compiled.error());
                if (expected && (compiled.value().request_digest != expected->request_digest ||
                                 compiled.value().plan->fingerprint != expected->plan_fingerprint))
                    return reject(ErrorEnvelope::make(ErrorCode::plan_stale));
                if (context.revoked || context.expires_at_ms <= state->now_ms() ||
                    !state->config.policy.enabled)
                    return reject(ErrorEnvelope::make(ErrorCode::permission_denied));
                if (!state->accepting || state->runs.size() >= state->config.max_runs)
                    return reject(ErrorEnvelope::make(ErrorCode::resource_unavailable));
                auto pins = verify_plan_pins(*compiled.value().plan, state->config.capabilities);
                if (!pins)
                    return reject(pins.error());
                const auto &plan = *compiled.value().plan;
                auto plan_authorization = state->gate.authorize_plan_admission(
                    context, state->config.policy, plan, state->config.capabilities,
                    state->now_ms());
                if (!plan_authorization)
                    return reject(plan_authorization.error());
                auto deadline = Deadline::after(
                    state->config.clock->now(),
                    std::chrono::milliseconds(
                        plan.single_root_inference
                            ? std::min(plan.limits.timeout_ms, plan.steps.front().timeout_ms)
                            : plan.limits.timeout_ms));
                if (!deadline)
                    return reject(deadline.error());
                auto known = state->gate.authorize_known_inputs(
                    context, state->config.policy, plan, state->config.capabilities,
                    compiled.value().input_values, state->now_ms(),
                    milliseconds(deadline.value().time()));
                if (!known)
                    return reject(known.error());
                LifecycleLimits limits;
                limits.jobs = plan.limits.max_jobs;
                limits.attempts = plan.limits.max_attempts;
                limits.suspensions = plan.limits.max_suspensions;
                limits.commands = plan.limits.max_control_commands;
                limits.event_slots = std::max<std::size_t>(32, plan.mandatory_event_bytes / 8192);
                auto controller = RunController::create(
                    ticket.run, limits, deadline.value(), *state->config.clock,
                    !plan.single_root_inference || plan.steps.front().pause_supported);
                if (!controller)
                    return reject(controller.error());
                auto run = std::make_unique<Impl::Run>(context, compiled.value().plan,
                                                       std::move(controller).value());
                auto charged = run->budget.reserve({.jobs = plan.static_jobs});
                if (!charged)
                    return reject(charged.error());
                auto root = run->controller->root();
                WorkflowInvocation invocation;
                invocation.job = root;
                invocation.deadline = deadline.value();
                if (plan.single_root_inference) {
                    const auto &step = plan.steps.front();
                    invocation.node = step.id;
                    invocation.child_envelope = step.child_envelope;
                    invocation.max_attempts = step.max_attempts;
                    invocation.backoff_ms = step.backoff_ms;
                    invocation.capability_pin = step.capability_pin;
                    invocation.effects = step.effects;
                    invocation.output_schema = step.output_schema;
                    invocation.input = arguments(step, compiled.value().input_values);
                } else {
                    auto workflow = WorkflowMachine::create(compiled.value().plan,
                                                            compiled.value().input_values,
                                                            *run->controller, *state->config.clock);
                    if (!workflow)
                        return reject(workflow.error());
                    run->workflow = std::move(workflow).value();
                    invocation.coordinating = true;
                    invocation.input = JsonValue::Object{};
                }
                auto queued = run->controller->queue(root);
                if (!queued)
                    return reject(queued.error());
                auto added = state->add_driver(*run, std::move(invocation));
                if (!added)
                    return reject(added.error());
                state->runs.emplace(ticket.run, std::move(run));
                auto admitted = state->submissions.admit(ticket, state->now_ms());
                if (!admitted) {
                    state->scheduler.remove(root);
                    state->runs.erase(ticket.run);
                    return reject(admitted.error());
                }
                pending->second.promise->set_value(
                    Result<SubmissionReceipt>::success({ticket.run, false}));
                (void)state->admission.settle_claim(pending->second.admission_claim);
                state->pending.erase(pending);
                return Result<void>::success();
            },
            true);
        if (!admission)
            return Result<SubmissionReceipt>::failure(admission.error());
    }
    auto result = preparation.value().decision.get();
    (void)state->call<void>(
        [state, ticket = preparation.value().ticket] {
            state->submissions.release_receipt(ticket);
            return Result<void>::success();
        },
        true);
    if (result && expected) {
        auto checked = state->call<void>([state, context, expected = *expected,
                                          run = result.value().id] {
            auto found = state->runs.find(run);
            if (found == state->runs.end() || found->second->context.subject != context.subject ||
                found->second->plan->fingerprint != expected.plan_fingerprint)
                return failure<void>(ErrorCode::plan_stale);
            return Result<void>::success();
        });
        if (!checked)
            return Result<SubmissionReceipt>::failure(checked.error());
    }
    if (result && !preparation.value().ticket.owner)
        result.value().duplicate = true;
    return result;
}
Result<RunSnapshot> RuntimeInstance::status(const AuthorizationContext &c, RunId id) {
    auto state = impl_;
    return state->call<RunSnapshot>([state, c, id] {
        auto it = state->runs.find(id);
        if (it == state->runs.end())
            return failure<RunSnapshot>(ErrorCode::invalid_reference);
        auto allowed = state->access(c, *it->second, AccessSurface::status);
        if (!allowed)
            return Result<RunSnapshot>::failure(allowed.error());
        return Result<RunSnapshot>::success(it->second->controller->snapshot());
    });
}
Result<RunResult> RuntimeInstance::result(const AuthorizationContext &c, RunId id) {
    auto state = impl_;
    return state->call<RunResult>([state, c, id] {
        auto it = state->runs.find(id);
        if (it == state->runs.end())
            return failure<RunResult>(ErrorCode::invalid_reference);
        auto allowed = state->access(c, *it->second, AccessSurface::result);
        if (!allowed)
            return Result<RunResult>::failure(allowed.error());
        const auto &run = *it->second;
        if (run.result.value && run.plan->single_root_inference) {
            const auto &step = run.plan->steps.front();
            if (!step.capability_pin)
                return failure<RunResult>(ErrorCode::invalid_result);
            auto checked = state->gate.check_retained_result_handles(
                c, state->config.policy, *step.capability_pin, step.output_schema,
                state->config.capabilities, *run.result.value, state->now_ms());
            if (!checked)
                return Result<RunResult>::failure(checked.error());
        } else if (run.result.value) {
            const auto *values = std::get_if<JsonValue::Object>(&run.result.value->data);
            if (!values || values->size() != run.plan->result_outputs.size())
                return failure<RunResult>(ErrorCode::invalid_result);
            for (const auto &[name, contract] : run.plan->result_outputs) {
                auto found = values->find(name);
                if (found == values->end() || !validate_value(found->second, contract.schema))
                    return failure<RunResult>(ErrorCode::invalid_result);
                if (contract.handle_validator) {
                    auto checked = state->gate.check_retained_result_handles(
                        c, state->config.policy, *contract.handle_validator, contract.schema,
                        state->config.capabilities, found->second, state->now_ms());
                    if (!checked)
                        return Result<RunResult>::failure(checked.error());
                }
            }
        }
        return Result<RunResult>::success(it->second->result);
    });
}
Result<CommandReceipt> RuntimeInstance::cancel(const AuthorizationContext &c, RunId id) {
    auto state = impl_;
    return state->call<CommandReceipt>(
        [state, c, id] {
            auto it = state->runs.find(id);
            if (it == state->runs.end())
                return failure<CommandReceipt>(ErrorCode::invalid_reference);
            auto allowed = state->access(c, *it->second, AccessSurface::cancel);
            if (!allowed)
                return Result<CommandReceipt>::failure(allowed.error());
            auto result = it->second->controller->cancel_run();
            if (!result)
                return Result<CommandReceipt>::failure(result.error());
            return Result<CommandReceipt>::success({true, true, !it->second->result.pending});
        },
        true);
}
Result<CommandReceipt> RuntimeInstance::pause(const AuthorizationContext &c, RunId id,
                                              std::uint64_t command) {
    auto state = impl_;
    return state->call<CommandReceipt>([state, c, id, command] {
        auto it = state->runs.find(id);
        if (it == state->runs.end())
            return failure<CommandReceipt>(ErrorCode::invalid_reference);
        auto allowed = state->access(c, *it->second, AccessSurface::resume);
        if (!allowed)
            return Result<CommandReceipt>::failure(allowed.error());
        auto charge = it->second->budget.reserve({.commands = 1});
        if (!charge)
            return Result<CommandReceipt>::failure(charge.error());
        auto deadline = Deadline::after(state->config.clock->now(), std::chrono::seconds(5));
        auto result = it->second->controller->pause_run(command, deadline.value());
        if (!result)
            return Result<CommandReceipt>::failure(result.error());
        return Result<CommandReceipt>::success(
            {true, !it->second->controller->snapshot().pause_barrier_pending, false});
    });
}
Result<CommandReceipt> RuntimeInstance::resume(const AuthorizationContext &c, RunId id,
                                               std::uint64_t command) {
    auto state = impl_;
    return state->call<CommandReceipt>([state, c, id, command] {
        auto it = state->runs.find(id);
        if (it == state->runs.end())
            return failure<CommandReceipt>(ErrorCode::invalid_reference);
        auto allowed = state->access(c, *it->second, AccessSurface::resume);
        if (!allowed)
            return Result<CommandReceipt>::failure(allowed.error());
        auto charge = it->second->budget.reserve({.commands = 1});
        if (!charge)
            return Result<CommandReceipt>::failure(charge.error());
        auto result = it->second->controller->resume_run(command, true);
        if (!result)
            return Result<CommandReceipt>::failure(result.error());
        it->second->context = attenuate(it->second->context, c);
        return Result<CommandReceipt>::success({true, true, false});
    });
}
Result<ObservationPage> RuntimeInstance::events(const AuthorizationContext &c, RunId id,
                                                std::uint64_t after, std::size_t limit) {
    auto state = impl_;
    return state->call<ObservationPage>([state, c, id, after, limit] {
        auto it = state->runs.find(id);
        if (it == state->runs.end())
            return failure<ObservationPage>(ErrorCode::invalid_reference);
        return AuthorizedObservation(it->second->controller->observation_store(),
                                     it->second->context.subject)
            .read(c, state->config.policy, state->now_ms(), after, limit);
    });
}
Result<ReplaySnapshot> RuntimeInstance::replay(const AuthorizationContext &c, RunId id) {
    auto state = impl_;
    return state->call<ReplaySnapshot>([state, c, id] {
        auto it = state->runs.find(id);
        if (it == state->runs.end())
            return failure<ReplaySnapshot>(ErrorCode::invalid_reference);
        return AuthorizedObservation(it->second->controller->observation_store(),
                                     it->second->context.subject)
            .replay(c, state->config.policy, state->now_ms());
    });
}
Result<void> RuntimeInstance::observe_host_envelope(HostEnvelope value) {
    auto state = impl_;
    return state->call<void>(
        [state, value] {
            auto result = state->resources.observe_envelope(value, state->config.clock->now());
            if (result) {
                auto previous = state->envelopes.find(value.resource);
                if (previous != state->envelopes.end() && previous->second.epoch != value.epoch)
                    state->residency.fence(previous->second.epoch);
                state->envelopes[value.resource] = value;
            }
            return result;
        },
        true);
}
Result<void> RuntimeInstance::observe_host_acquisition(HostAcquisition value) {
    auto state = impl_;
    return state->call<void>([state, value] { return state->resources.acquired(value); }, true);
}
Result<void> RuntimeInstance::observe_host_release(RuntimeHostRelease value) {
    auto state = impl_;
    return state->call<void>(
        [state, value] {
            return state->resources.observe_host_release(value, state->config.clock->now());
        },
        true);
}
Result<void> RuntimeInstance::replace_policy(PolicySnapshot value) {
    auto state = impl_;
    return state->call<void>(
        [state, value = std::move(value)]() mutable {
            if (value.revision <= state->config.policy.revision)
                return failure<void>(ErrorCode::invalid_request);
            state->config.policy = std::move(value);
            for (auto &[id, run] : state->runs) {
                (void)id;
                for (auto &[job, d] : run->drivers) {
                    (void)job;
                    if (d.provider.valid()) {
                        auto current = state->authorize(*run, d, AuthorizationBoundary::dispatch);
                        if (!current)
                            state->stop(*run, d, current.error());
                    }
                }
            }
            return Result<void>::success();
        },
        true);
}
Result<void> RuntimeInstance::replace_capabilities(CapabilitySnapshot value) {
    auto state = impl_;
    return state->call<void>(
        [state, value = std::move(value)]() mutable {
            auto bound = bind_registration_contracts(value, state->config.registrations);
            if (!bound)
                return bound;
            state->config.capabilities = std::move(value);
            return Result<void>::success();
        },
        true);
}
Result<ResourceSnapshot> RuntimeInstance::resource_snapshot(LogicalResourceId resource) {
    auto state = impl_;
    return state->call<ResourceSnapshot>([state, resource] {
        return Result<ResourceSnapshot>::success(state->resources.snapshot(resource));
    });
}
Result<void> RuntimeInstance::poll() {
    auto state = impl_;
    return state->call<void>(
        [state] {
            state->progress();
            return Result<void>::success();
        },
        true);
}
Result<CommandReceipt> RuntimeInstance::propose_children(const AuthorizationContext &context,
                                                         RunId id, JobId job,
                                                         std::string_view json) {
    auto state = impl_;
    struct Preparation {
        ChildEnvelope envelope;
        RunLimits remaining;
        CapabilitySnapshot registry;
        std::uint64_t depth, generation;
    };
    auto prepared = state->call<Preparation>([state, context, id, job]() -> Result<Preparation> {
        auto it = state->runs.find(id);
        if (it == state->runs.end())
            return failure<Preparation>(ErrorCode::invalid_reference);
        auto &run = *it->second;
        auto permitted = state->access(context, run, AccessSurface::inject);
        if (!permitted)
            return Result<Preparation>::failure(permitted.error());
        auto charged = run.budget.reserve({.proposals = 1, .commands = 1});
        if (!charged)
            return Result<Preparation>::failure(charged.error());
        auto driver = run.drivers.find(job);
        if (driver == run.drivers.end())
            return failure<Preparation>(ErrorCode::invalid_reference);
        auto &d = driver->second;
        auto job_state = run.controller->job(job);
        if (!job_state || job_state.value().state != JobState::running || !d.native ||
            !d.registration->native->options.injection_slot || d.proposal ||
            !run.controller->snapshot().child_creation_open)
            return failure<Preparation>(ErrorCode::invalid_request);
        if (!d.invocation.child_envelope)
            return failure<Preparation>(ErrorCode::permission_denied);
        auto current = state->authorize(run, d, AuthorizationBoundary::dispatch);
        if (!current)
            return Result<Preparation>::failure(current.error());
        auto remaining = run.budget.remaining();
        const auto now = state->config.clock->now();
        if (d.invocation.deadline.expired(now))
            return failure<Preparation>(ErrorCode::job_timeout);
        remaining.timeout_ms = milliseconds(d.invocation.deadline.time() - now);
        return Result<Preparation>::success({*d.invocation.child_envelope, remaining,
                                             state->config.capabilities, d.child_depth + 1,
                                             job_state.value().dispatch_generation});
    });
    if (!prepared)
        return Result<CommandReceipt>::failure(prepared.error());
    auto compiled =
        compile_child_fragment(json, prepared.value().registry, prepared.value().envelope,
                               prepared.value().remaining, prepared.value().depth);
    return state->call<CommandReceipt>(
        [state, context, id, job, generation = prepared.value().generation,
         compiled = std::move(compiled)]() mutable -> Result<CommandReceipt> {
            if (!compiled)
                return Result<CommandReceipt>::failure(compiled.error());
            auto it = state->runs.find(id);
            if (it == state->runs.end())
                return failure<CommandReceipt>(ErrorCode::invalid_reference);
            auto &run = *it->second;
            auto permitted = state->access(context, run, AccessSurface::inject);
            if (!permitted)
                return Result<CommandReceipt>::failure(permitted.error());
            auto driver = run.drivers.find(job);
            if (driver == run.drivers.end())
                return failure<CommandReceipt>(ErrorCode::invalid_reference);
            auto &d = driver->second;
            auto job_state = run.controller->job(job);
            if (!job_state || job_state.value().state != JobState::running ||
                job_state.value().dispatch_generation != generation || d.proposal ||
                !run.controller->snapshot().child_creation_open)
                return failure<CommandReceipt>(ErrorCode::invalid_request);
            const auto effective =
                attenuate(attenuate(run.admission_context, run.context), context);
            auto auth = state->gate.authorize_plan_admission(
                effective, state->config.policy, *compiled.value().plan, state->config.capabilities,
                state->now_ms());
            if (!auth)
                return Result<CommandReceipt>::failure(auth.error());
            auto known = state->gate.authorize_known_inputs(
                effective, state->config.policy, *compiled.value().plan, state->config.capabilities,
                compiled.value().input_values, state->now_ms(),
                milliseconds(d.invocation.deadline.time()));
            if (!known)
                return Result<CommandReceipt>::failure(known.error());
            auto budget = run.budget;
            auto charge =
                budget.reserve({.jobs = compiled.value().plan->static_jobs, .suspensions = 1});
            if (!charge)
                return Result<CommandReceipt>::failure(charge.error());
            d.proposal = Impl::Proposal{std::move(compiled).value(), generation};
            return Result<CommandReceipt>::success({true, false, false});
        });
}
Result<std::uint64_t> RuntimeInstance::admission_epoch() {
    auto state = impl_;
    return state->call<std::uint64_t>(
        [state] { return Result<std::uint64_t>::success(state->admission.epoch()); });
}
Result<std::uint64_t> RuntimeInstance::prepare_idle_stop(std::uint64_t expected) {
    auto state = impl_;
    return state->call<std::uint64_t>(
        [state, expected] {
            IdleSnapshot idle;
            idle.duplicate_waiters = state->pending.size();
            for (const auto &[id, run] : state->runs) {
                (void)id;
                for (const auto &job : run->controller->snapshot().jobs)
                    if (!is_terminal(job.state))
                        ++idle.nonterminal_jobs;
                for (const auto &[job, d] : run->drivers) {
                    (void)job;
                    if (d.reservation)
                        ++idle.prepared_dispatches;
                    if (d.preparing_native.valid())
                        ++idle.model_loads;
                    if (d.provider.valid())
                        ++idle.nontransferable_cleanup;
                }
            }
            for (const auto &[resource, envelope] : state->envelopes) {
                (void)envelope;
                auto ledger = state->resources.snapshot(resource);
                idle.nontransferable_cleanup += ledger.cleanup_records + ledger.allocations;
            }
            auto stopped = state->admission.prepare_idle_stop(expected, idle);
            if (stopped)
                state->accepting = false;
            return stopped;
        },
        true);
}
Result<void> RuntimeInstance::fence_admission() {
    auto state = impl_;
    return state->call<void>(
        [state] {
            state->accepting = false;
            auto drained = state->admission.begin_drain();
            for (const auto &[resource, envelope] : state->envelopes) {
                state->resources.fence(resource);
                state->residency.fence(envelope.epoch);
            }
            for (auto &[id, run] : state->runs) {
                (void)id;
                if (run->result.pending)
                    (void)run->controller->cancel_run(ErrorCode::capability_unavailable);
            }
            return drained ? Result<void>::success() : Result<void>::failure(drained.error());
        },
        true);
}
std::weak_ptr<RuntimeCleanupEndpoint> RuntimeInstance::cleanup_endpoint() const noexcept {
    return impl_;
}
Result<RuntimeDrainSnapshot> RuntimeInstance::drain_status() {
    auto state = impl_;
    return state->call<RuntimeDrainSnapshot>(
        [state] { return Result<RuntimeDrainSnapshot>::success(state->drain_snapshot()); }, true);
}
Result<void> RuntimeInstance::shutdown() {
    auto state = impl_;
    return state->call<void>(
        [state] {
            state->accepting = false;
            if (!state->drain_deadline) {
                auto drain =
                    Deadline::after(state->config.clock->now(), state->config.drain_timeout);
                if (!drain)
                    return Result<void>::failure(drain.error());
                auto cleanup = Deadline::after(drain.value().time(), state->config.cleanup_timeout);
                if (!cleanup)
                    return Result<void>::failure(cleanup.error());
                state->drain_deadline = drain.value().time();
                state->cleanup_deadline = cleanup.value().time();
            }
            (void)state->admission.begin_drain();
            for (auto &[id, claim] : state->pending) {
                (void)id;
                auto error = ErrorEnvelope::make(ErrorCode::run_cancelled);
                (void)state->submissions.reject(claim.ticket, error);
                claim.promise->set_value(Result<SubmissionReceipt>::failure(error));
                (void)state->admission.settle_claim(claim.admission_claim);
            }
            state->pending.clear();
            for (auto &[id, run] : state->runs) {
                (void)id;
                if (run->result.pending)
                    (void)run->controller->cancel_run();
            }
            return Result<void>::success();
        },
        true);
}
} // namespace flamoris::runtime
