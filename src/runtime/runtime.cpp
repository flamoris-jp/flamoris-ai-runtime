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
} // namespace
struct RuntimeInstance::Impl final : RunObservationLookupPort {
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
        std::optional<NativeObservation> retained_observation;
        std::optional<Proposal> proposal;
        std::unique_ptr<WorkflowMachine> fragment;
        WorkflowMachine *owner_machine{};
        std::optional<JobId> dynamic_parent;
        std::optional<std::string> injection;
        std::uint64_t injection_id{}, child_depth{};
        std::shared_ptr<const ExecutionPlan> execution_plan;
        std::unique_ptr<InferenceMachine> native;
        std::future<Result<std::unique_ptr<NativeWorkerPort>>> preparing_native;
        std::future<AdapterOutcome> provider;
        std::optional<JsonValue> output;
        std::optional<ErrorEnvelope> error;
        ExternalOutcome outcome{ExternalOutcome::not_applicable};
        std::string text;
        bool native_loaded{}, native_released{}, native_quiescent{}, release_requested{},
            host_released{}, published{}, resuming{};
    };
    struct Run {
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
            : context(std::move(c)), plan(std::move(p)), controller(std::move(ctl)),
              budget(plan->limits) {}
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
    std::map<RunId, std::unique_ptr<Run>> runs;
    std::map<std::string, std::shared_ptr<PersistentAdapterBinding>> adapters;
    std::map<PendingSubmissionId, ClaimWait> pending;
    std::map<LogicalResourceId, HostEnvelope> envelopes;
    CheckedCounter operations;
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<std::function<void()>> normal, mandatory;
    bool stopping{}, accepting{true};
    std::thread actor;

    explicit Impl(RuntimeConfiguration value)
        : config(std::move(value)), submissions(config.instance, config.submissions),
          admission(1, config.normal_commands), compiler(config.compiler),
          scheduler(config.max_runs * config.compiler.limits.max_jobs),
          cleanup(*this, *config.clock, 128), resources(config.instance, *config.host, cleanup) {
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
                if ((d.native && !d.native_released) || d.provider.valid() ||
                    d.preparing_native.valid())
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
                (void)run.controller->observe_stopped(
                    *d.ticket, {true, false, ExternalOutcome::not_dispatched});
            }
        }
    }
    void enqueue(Run &run, JobId job, const Driver &d) {
        ReadyJob ready;
        ready.job = job;
        ready.priority = 1;
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
        if (observation.operation == NativeOperation::step && !observation.control_rejected) {
            const auto state = run.controller->job(d.invocation.job);
            if (state && state.value().state == JobState::running) {
                if (observation.segment.text.size() >
                    run.plan->limits.max_output_bytes - d.text.size())
                    stop(run, d, ErrorEnvelope::make(ErrorCode::result_too_large));
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
            if (auth && pins && run.controller->snapshot().child_creation_open &&
                d.proposal->dispatch_generation == observation.ticket.dispatch_generation) {
                auto budget = run.budget;
                auto charged = budget.reserve(
                    {.jobs = d.proposal->compiled.plan->static_jobs, .suspensions = 1});
                const std::array specs{ChildSpec{d.invocation.deadline, true}};
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
                    invocation.deadline = d.invocation.deadline;
                    if (compiled.plan->single_root_inference) {
                        const auto &step = compiled.plan->steps.front();
                        invocation.node = step.id;
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
        }
        auto accepted = d.native->accept(*run.controller, observation);
        if (!accepted) {
            d.retained_observation = std::move(observation);
            return;
        }
        d.retained_observation.reset();
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
            materialize(observation.model_bytes, d.model_allocation);
            materialize(observation.state_bytes, d.state_allocation);
            if (d.model_allocation && d.state_allocation) {
                const std::array manifest{*d.model_allocation, *d.state_allocation};
                auto complete = resources.materialization_complete(*d.reservation, manifest,
                                                                   config.clock->now());
                if (!complete)
                    stop(run, d, complete.error());
            }
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
            release_allocation(d.model_allocation, observation.model_allocation_released);
            request_host_release(d);
        }
    }
    void consume(Run &run, Driver &d) {
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
            d.outcome = outcome.external_outcome;
            d.native_quiescent = true;
            if (outcome.error) {
                stop(run, d, *outcome.error);
                (void)run.controller->observe_stopped(*d.ticket, {true, false, d.outcome});
            } else {
                d.output = std::move(outcome.value);
                auto state = run.controller->job(d.invocation.job);
                if (state && state.value().state == JobState::cancelling)
                    (void)run.controller->observe_stopped(*d.ticket, {true, false, d.outcome});
                else
                    (void)run.controller->complete(*d.ticket, d.outcome);
            }
            request_host_release(d);
        }
        release_ledger(d);
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
        d.published = true;
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
            run.result.pending = false;
            run.terminal_since = config.clock->now();
            run.result.external_outcome = d.outcome;
            if (job.value().state == JobState::succeeded)
                run.result.value = d.output;
            else
                run.result.error = d.error.value_or(
                    ErrorEnvelope::make(job.value().error.value_or(ErrorCode::internal_error)));
        } else if (d.owner_machine) {
            if (job.value().state == JobState::succeeded && d.output)
                (void)d.owner_machine->accept_result(d.invocation.job, *d.output);
            else
                (void)d.owner_machine->accept_failure(
                    d.invocation.job, d.error.value_or(ErrorEnvelope::make(
                                          job.value().error.value_or(ErrorCode::internal_error))));
        }
    }
    void native_step(Run &run, Driver &d, const JobSnapshot &state) {
        if (!d.native || d.native->pending() || !d.ticket)
            return;
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
        if (workers() >= config.max_workers && !d.native && !d.provider.valid() &&
            !d.preparing_native.valid())
            return;
        auto authorized = authorize(run, d,
                                    job.value().pending_resume ? AuthorizationBoundary::resume
                                                               : AuthorizationBoundary::dispatch);
        if (!authorized) {
            stop(run, d, authorized.error());
            return;
        }
        if (!d.reservation) {
            auto env = envelopes.find(d.registration->resource);
            if (env == envelopes.end())
                return;
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
            if (d.native_loaded) {
                request.incremental[ResourceKind::ram] = 0;
                request.incremental[ResourceKind::device] = 0;
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
        auto lease = resources.commit_dispatch(*d.reservation, {true, true, true, true},
                                               config.clock->now());
        if (!lease)
            return;
        d.lease = lease.value();
        auto charged = run.budget.reserve({.attempts = job.value().attempt ? 0ULL : 1ULL});
        if (!charged) {
            stop(run, d, charged.error());
            d.native_quiescent = true;
            request_host_release(d);
            return;
        }
        auto ticket = run.controller->dispatch(d.invocation.job, {true, true, true, true});
        if (!ticket) {
            stop(run, d, ticket.error());
            d.native_quiescent = true;
            request_host_release(d);
            return;
        }
        d.ticket = ticket.value();
        d.resuming = job.value().pending_resume;
        scheduler.remove(d.invocation.job);
        if (d.registration->native && !d.native) {
            auto model_id = resources.next_allocation_identity(*d.reservation);
            auto state_id = resources.next_allocation_identity(*d.reservation);
            if (!model_id || !state_id) {
                stop(run, d, ErrorEnvelope::make(ErrorCode::resource_unavailable));
                d.native_quiescent = true;
                request_host_release(d);
                return;
            }
            d.prepared_model = model_id.value();
            d.prepared_state = state_id.value();
            auto config_copy = *d.registration->native;
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
            request.now_ms = now_ms();
            request.deadline_ms = milliseconds(d.invocation.deadline.time());
            request.concrete_inputs = std::get<JsonValue::Object>(d.invocation.input.data);
            request.concrete_input_digest = authorized.value().concrete_input_digest;
            auto grant = adapter_authority.authorize(
                run.context, config.policy, *run.plan, config.capabilities, request, *d.ticket,
                *d.invocation.capability_pin, d.invocation.input,
                std::to_string(config.instance.high()) + ":" +
                    std::to_string(config.instance.low()) + ":" +
                    std::to_string(run.controller->id().value()) + ":" +
                    std::to_string(d.invocation.job.value()));
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
                        if (driver.native)
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
                if (d != run->second->drivers.end())
                    dispatch(*run->second, d->second);
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

RuntimeInstance::RuntimeInstance(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
    impl_->actor = std::thread([state = impl_.get()] { state->loop(); });
}
Result<std::unique_ptr<RuntimeInstance>> RuntimeInstance::create(RuntimeConfiguration config) {
    try {
        if (!config.max_runs || !config.normal_commands || !config.mandatory_commands ||
            !config.max_workers || config.max_runs > 1024 || config.registrations.size() > 256)
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
        auto instance = std::unique_ptr<RuntimeInstance>(
            new RuntimeInstance(std::make_unique<Impl>(std::move(config))));
        return Result<std::unique_ptr<RuntimeInstance>>::success(std::move(instance));
    } catch (...) {
        return failure<std::unique_ptr<RuntimeInstance>>(ErrorCode::resource_unavailable);
    }
}
RuntimeInstance::~RuntimeInstance() {
    (void)shutdown();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->condition.notify_all();
    if (impl_->actor.joinable())
        impl_->actor.join();
    // All mutable controllers are now quiescent. Driver futures/native worker owners
    // join outside the actor before controllers, host ports and ledger are destroyed.
    for (auto &[id, run] : impl_->runs) {
        (void)id;
        for (auto &[job, d] : run->drivers) {
            (void)job;
            if (d.provider.valid())
                d.provider.wait();
            if (d.preparing_native.valid())
                d.preparing_native.wait();
            d.native.reset();
        }
    }
}
RuntimeInstanceId RuntimeInstance::instance_id() const noexcept { return impl_->config.instance; }
Result<SubmissionReceipt> RuntimeInstance::submit(const AuthorizationContext &context,
                                                  std::string_view json) {
    if (json.size() > 1048576)
        return failure<SubmissionReceipt>(ErrorCode::invalid_request);
    auto identity = validate_submission_identity(json);
    if (!identity)
        return Result<SubmissionReceipt>::failure(identity.error());
    struct Preparation {
        SubmissionTicket ticket;
        std::shared_future<Result<SubmissionReceipt>> decision;
        CapabilitySnapshot registry;
    };
    auto preparation = impl_->call<Preparation>(
        [this, context, identity = identity.value()]() mutable -> Result<Preparation> {
            if (!impl_->accepting || context.subject.empty() || context.subject.size() > 128 ||
                context.revoked || impl_->now_ms() >= context.expires_at_ms ||
                !impl_->config.policy.enabled)
                return failure<Preparation>(ErrorCode::permission_denied);
            auto ticket = impl_->submissions.claim(context.subject, identity.request_kind,
                                                   identity.idempotency_key,
                                                   identity.request_digest, impl_->now_ms());
            if (!ticket)
                return Result<Preparation>::failure(ticket.error());
            if (!ticket.value().owner) {
                auto existing = impl_->submissions.poll(ticket.value());
                if (existing && existing.value() && existing.value()->run) {
                    auto promise = std::make_shared<std::promise<Result<SubmissionReceipt>>>();
                    auto future = promise->get_future().share();
                    promise->set_value(
                        Result<SubmissionReceipt>::success({*existing.value()->run, true}));
                    impl_->submissions.release_receipt(ticket.value());
                    return Result<Preparation>::success({ticket.value(), future, {}});
                }
                auto pending = impl_->pending.find(ticket.value().pending);
                if (pending == impl_->pending.end())
                    return failure<Preparation>(ErrorCode::state_unavailable);
                return Result<Preparation>::success({ticket.value(), pending->second.future, {}});
            }
            auto admission_claim = impl_->admission.claim();
            if (!admission_claim) {
                (void)impl_->submissions.reject(ticket.value(), admission_claim.error());
                return Result<Preparation>::failure(admission_claim.error());
            }
            auto promise = std::make_shared<std::promise<Result<SubmissionReceipt>>>();
            auto future = promise->get_future().share();
            impl_->pending.emplace(
                ticket.value().pending,
                Impl::ClaimWait{ticket.value(), promise, future, admission_claim.value()});
            return Result<Preparation>::success(
                {ticket.value(), future, impl_->config.capabilities});
        });
    if (!preparation)
        return Result<SubmissionReceipt>::failure(preparation.error());
    if (impl_->config.submission_preparation_hook) {
        try {
            impl_->config.submission_preparation_hook(preparation.value().ticket.pending,
                                                      preparation.value().ticket.owner);
        } catch (...) {
            if (preparation.value().ticket.owner)
                (void)impl_->call<void>(
                    [this, ticket = preparation.value().ticket] {
                        auto pending = impl_->pending.find(ticket.pending);
                        if (pending != impl_->pending.end()) {
                            auto error = ErrorEnvelope::make(ErrorCode::internal_error);
                            (void)impl_->submissions.reject(ticket, error);
                            pending->second.promise->set_value(
                                Result<SubmissionReceipt>::failure(error));
                            (void)impl_->admission.settle_claim(pending->second.admission_claim);
                            impl_->pending.erase(pending);
                        }
                        return Result<void>::success();
                    },
                    true);
            return failure<SubmissionReceipt>(ErrorCode::internal_error);
        }
    }
    if (preparation.value().ticket.owner) {
        auto compiled = impl_->compiler.compile_submission(json, preparation.value().registry);
        auto admission = impl_->call<void>(
            [this, context, ticket = preparation.value().ticket,
             compiled = std::move(compiled)]() mutable -> Result<void> {
                auto pending = impl_->pending.find(ticket.pending);
                if (pending == impl_->pending.end())
                    return failure<void>(ErrorCode::state_unavailable);
                Impl::PendingFailureGuard resolution{*impl_, ticket};
                auto reject = [&](ErrorEnvelope error) {
                    (void)impl_->submissions.reject(ticket, error);
                    pending->second.promise->set_value(Result<SubmissionReceipt>::failure(error));
                    (void)impl_->admission.settle_claim(pending->second.admission_claim);
                    impl_->pending.erase(pending);
                    return Result<void>::success();
                };
                if (!compiled)
                    return reject(compiled.error());
                if (context.revoked || context.expires_at_ms <= impl_->now_ms() ||
                    !impl_->config.policy.enabled)
                    return reject(ErrorEnvelope::make(ErrorCode::permission_denied));
                if (!impl_->accepting || impl_->runs.size() >= impl_->config.max_runs)
                    return reject(ErrorEnvelope::make(ErrorCode::resource_unavailable));
                auto pins = verify_plan_pins(*compiled.value().plan, impl_->config.capabilities);
                if (!pins)
                    return reject(pins.error());
                const auto &plan = *compiled.value().plan;
                auto plan_authorization = impl_->gate.authorize_plan_admission(
                    context, impl_->config.policy, plan, impl_->config.capabilities,
                    impl_->now_ms());
                if (!plan_authorization)
                    return reject(plan_authorization.error());
                auto deadline = Deadline::after(impl_->config.clock->now(),
                                                std::chrono::milliseconds(plan.limits.timeout_ms));
                if (!deadline)
                    return reject(deadline.error());
                for (const auto &step : plan.steps) {
                    if (!step.capability_pin)
                        continue;
                    bool known = true;
                    for (const auto &[name, binding] : step.inputs) {
                        (void)name;
                        if (binding.reference &&
                            binding.reference->source == Reference::Source::node)
                            known = false;
                    }
                    if (!known)
                        continue;
                    AuthorizationRequest request;
                    request.boundary = AuthorizationBoundary::admission;
                    request.capability = step.capability_pin->identifier;
                    request.concrete_inputs = arguments(step, compiled.value().input_values);
                    request.now_ms = impl_->now_ms();
                    request.deadline_ms = milliseconds(deadline.value().time());
                    auto authorized = impl_->gate.check(context, impl_->config.policy, plan,
                                                        impl_->config.capabilities, request);
                    if (!authorized)
                        return reject(authorized.error());
                }
                LifecycleLimits limits;
                limits.jobs = plan.limits.max_jobs;
                limits.attempts = plan.limits.max_attempts;
                limits.suspensions = plan.limits.max_suspensions;
                limits.commands = plan.limits.max_control_commands;
                limits.event_slots = std::max<std::size_t>(32, plan.mandatory_event_bytes / 8192);
                auto controller = RunController::create(
                    ticket.run, limits, deadline.value(), *impl_->config.clock,
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
                    invocation.capability_pin = step.capability_pin;
                    invocation.effects = step.effects;
                    invocation.output_schema = step.output_schema;
                    invocation.input = arguments(step, compiled.value().input_values);
                } else {
                    auto workflow = WorkflowMachine::create(compiled.value().plan,
                                                            compiled.value().input_values,
                                                            *run->controller, *impl_->config.clock);
                    if (!workflow)
                        return reject(workflow.error());
                    run->workflow = std::move(workflow).value();
                    invocation.coordinating = true;
                    invocation.input = JsonValue::Object{};
                }
                auto queued = run->controller->queue(root);
                if (!queued)
                    return reject(queued.error());
                auto added = impl_->add_driver(*run, std::move(invocation));
                if (!added)
                    return reject(added.error());
                impl_->runs.emplace(ticket.run, std::move(run));
                auto admitted = impl_->submissions.admit(ticket, impl_->now_ms());
                if (!admitted)
                    return reject(admitted.error());
                pending->second.promise->set_value(
                    Result<SubmissionReceipt>::success({ticket.run, false}));
                (void)impl_->admission.settle_claim(pending->second.admission_claim);
                impl_->pending.erase(pending);
                return Result<void>::success();
            },
            true);
        if (!admission)
            return Result<SubmissionReceipt>::failure(admission.error());
    }
    auto result = preparation.value().decision.get();
    (void)impl_->call<void>(
        [this, ticket = preparation.value().ticket] {
            impl_->submissions.release_receipt(ticket);
            return Result<void>::success();
        },
        true);
    if (result && !preparation.value().ticket.owner)
        result.value().duplicate = true;
    return result;
}
Result<RunSnapshot> RuntimeInstance::status(const AuthorizationContext &c, RunId id) {
    return impl_->call<RunSnapshot>([this, c, id] {
        auto it = impl_->runs.find(id);
        if (it == impl_->runs.end())
            return failure<RunSnapshot>(ErrorCode::invalid_reference);
        auto allowed = impl_->access(c, *it->second, AccessSurface::status);
        if (!allowed)
            return Result<RunSnapshot>::failure(allowed.error());
        return Result<RunSnapshot>::success(it->second->controller->snapshot());
    });
}
Result<RunResult> RuntimeInstance::result(const AuthorizationContext &c, RunId id) {
    return impl_->call<RunResult>([this, c, id] {
        auto it = impl_->runs.find(id);
        if (it == impl_->runs.end())
            return failure<RunResult>(ErrorCode::invalid_reference);
        auto allowed = impl_->access(c, *it->second, AccessSurface::result);
        if (!allowed)
            return Result<RunResult>::failure(allowed.error());
        return Result<RunResult>::success(it->second->result);
    });
}
Result<CommandReceipt> RuntimeInstance::cancel(const AuthorizationContext &c, RunId id) {
    return impl_->call<CommandReceipt>(
        [this, c, id] {
            auto it = impl_->runs.find(id);
            if (it == impl_->runs.end())
                return failure<CommandReceipt>(ErrorCode::invalid_reference);
            auto allowed = impl_->access(c, *it->second, AccessSurface::cancel);
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
    return impl_->call<CommandReceipt>([this, c, id, command] {
        auto it = impl_->runs.find(id);
        if (it == impl_->runs.end())
            return failure<CommandReceipt>(ErrorCode::invalid_reference);
        auto allowed = impl_->access(c, *it->second, AccessSurface::resume);
        if (!allowed)
            return Result<CommandReceipt>::failure(allowed.error());
        auto deadline = Deadline::after(impl_->config.clock->now(), std::chrono::seconds(5));
        auto result = it->second->controller->pause_run(command, deadline.value());
        if (!result)
            return Result<CommandReceipt>::failure(result.error());
        return Result<CommandReceipt>::success(
            {true, !it->second->controller->snapshot().pause_barrier_pending, false});
    });
}
Result<CommandReceipt> RuntimeInstance::resume(const AuthorizationContext &c, RunId id,
                                               std::uint64_t command) {
    return impl_->call<CommandReceipt>([this, c, id, command] {
        auto it = impl_->runs.find(id);
        if (it == impl_->runs.end())
            return failure<CommandReceipt>(ErrorCode::invalid_reference);
        auto allowed = impl_->access(c, *it->second, AccessSurface::resume);
        if (!allowed)
            return Result<CommandReceipt>::failure(allowed.error());
        auto result = it->second->controller->resume_run(command, true);
        if (!result)
            return Result<CommandReceipt>::failure(result.error());
        it->second->context = c;
        return Result<CommandReceipt>::success({true, true, false});
    });
}
Result<ObservationPage> RuntimeInstance::events(const AuthorizationContext &c, RunId id,
                                                std::uint64_t after, std::size_t limit) {
    return impl_->call<ObservationPage>([this, c, id, after, limit] {
        auto it = impl_->runs.find(id);
        if (it == impl_->runs.end())
            return failure<ObservationPage>(ErrorCode::invalid_reference);
        return AuthorizedObservation(it->second->controller->observation_store(),
                                     it->second->context.subject)
            .read(c, impl_->config.policy, impl_->now_ms(), after, limit);
    });
}
Result<ReplaySnapshot> RuntimeInstance::replay(const AuthorizationContext &c, RunId id) {
    return impl_->call<ReplaySnapshot>([this, c, id] {
        auto it = impl_->runs.find(id);
        if (it == impl_->runs.end())
            return failure<ReplaySnapshot>(ErrorCode::invalid_reference);
        return AuthorizedObservation(it->second->controller->observation_store(),
                                     it->second->context.subject)
            .replay(c, impl_->config.policy, impl_->now_ms());
    });
}
Result<void> RuntimeInstance::observe_host_envelope(HostEnvelope value) {
    return impl_->call<void>(
        [this, value] {
            auto result = impl_->resources.observe_envelope(value, impl_->config.clock->now());
            if (result)
                impl_->envelopes[value.resource] = value;
            return result;
        },
        true);
}
Result<void> RuntimeInstance::observe_host_acquisition(HostAcquisition value) {
    return impl_->call<void>([this, value] { return impl_->resources.acquired(value); }, true);
}
Result<void> RuntimeInstance::observe_host_release(RuntimeHostRelease value) {
    return impl_->call<void>(
        [this, value] {
            return impl_->resources.observe_host_release(value, impl_->config.clock->now());
        },
        true);
}
Result<void> RuntimeInstance::replace_policy(PolicySnapshot value) {
    return impl_->call<void>(
        [this, value = std::move(value)]() mutable {
            if (value.revision <= impl_->config.policy.revision)
                return failure<void>(ErrorCode::invalid_request);
            impl_->config.policy = std::move(value);
            return Result<void>::success();
        },
        true);
}
Result<void> RuntimeInstance::replace_capabilities(CapabilitySnapshot value) {
    return impl_->call<void>(
        [this, value = std::move(value)]() mutable {
            impl_->config.capabilities = std::move(value);
            return Result<void>::success();
        },
        true);
}
Result<ResourceSnapshot> RuntimeInstance::resource_snapshot(LogicalResourceId resource) {
    return impl_->call<ResourceSnapshot>([this, resource] {
        return Result<ResourceSnapshot>::success(impl_->resources.snapshot(resource));
    });
}
Result<void> RuntimeInstance::poll() {
    return impl_->call<void>(
        [this] {
            impl_->progress();
            return Result<void>::success();
        },
        true);
}
Result<CommandReceipt> RuntimeInstance::propose_children(const AuthorizationContext &context,
                                                         RunId id, JobId job,
                                                         std::string_view json) {
    struct Preparation {
        ChildEnvelope envelope;
        RunLimits remaining;
        CapabilitySnapshot registry;
        std::uint64_t depth, generation;
    };
    auto prepared = impl_->call<Preparation>([this, context, id, job]() -> Result<Preparation> {
        auto it = impl_->runs.find(id);
        if (it == impl_->runs.end())
            return failure<Preparation>(ErrorCode::invalid_reference);
        auto &run = *it->second;
        auto permitted = impl_->access(context, run, AccessSurface::inject);
        if (!permitted)
            return Result<Preparation>::failure(permitted.error());
        auto charged = run.budget.reserve({.proposals = 1, .commands = 1});
        if (!charged)
            return Result<Preparation>::failure(charged.error());
        auto driver = run.drivers.find(job);
        if (driver == run.drivers.end())
            return failure<Preparation>(ErrorCode::invalid_reference);
        auto &d = driver->second;
        auto state = run.controller->job(job);
        if (!state || state.value().state != JobState::running || !d.native ||
            !d.registration->native->options.injection_slot || d.proposal ||
            !run.controller->snapshot().child_creation_open)
            return failure<Preparation>(ErrorCode::invalid_request);
        const PlanStep *parent = nullptr;
        std::function<void(const std::vector<PlanStep> &)> find = [&](const auto &steps) {
            for (const auto &step : steps) {
                if (step.id == d.invocation.node)
                    parent = &step;
                find(step.members);
            }
        };
        find(d.execution_plan->steps);
        if (!parent || !parent->child_envelope)
            return failure<Preparation>(ErrorCode::permission_denied);
        auto current = impl_->authorize(run, d, AuthorizationBoundary::dispatch);
        if (!current)
            return Result<Preparation>::failure(current.error());
        auto remaining = run.budget.remaining();
        const auto now = impl_->config.clock->now();
        if (d.invocation.deadline.expired(now))
            return failure<Preparation>(ErrorCode::job_timeout);
        remaining.timeout_ms = milliseconds(d.invocation.deadline.time() - now);
        return Result<Preparation>::success({*parent->child_envelope, remaining,
                                             impl_->config.capabilities, d.child_depth + 1,
                                             state.value().dispatch_generation});
    });
    if (!prepared)
        return Result<CommandReceipt>::failure(prepared.error());
    auto compiled =
        compile_child_fragment(json, prepared.value().registry, prepared.value().envelope,
                               prepared.value().remaining, prepared.value().depth);
    return impl_->call<CommandReceipt>(
        [this, context, id, job, generation = prepared.value().generation,
         compiled = std::move(compiled)]() mutable -> Result<CommandReceipt> {
            if (!compiled)
                return Result<CommandReceipt>::failure(compiled.error());
            auto it = impl_->runs.find(id);
            if (it == impl_->runs.end())
                return failure<CommandReceipt>(ErrorCode::invalid_reference);
            auto &run = *it->second;
            auto permitted = impl_->access(context, run, AccessSurface::inject);
            if (!permitted)
                return Result<CommandReceipt>::failure(permitted.error());
            auto driver = run.drivers.find(job);
            if (driver == run.drivers.end())
                return failure<CommandReceipt>(ErrorCode::invalid_reference);
            auto &d = driver->second;
            auto state = run.controller->job(job);
            if (!state || state.value().state != JobState::running ||
                state.value().dispatch_generation != generation || d.proposal ||
                !run.controller->snapshot().child_creation_open)
                return failure<CommandReceipt>(ErrorCode::invalid_request);
            auto auth = impl_->gate.authorize_plan_admission(
                context, impl_->config.policy, *compiled.value().plan, impl_->config.capabilities,
                impl_->now_ms());
            if (!auth)
                return Result<CommandReceipt>::failure(auth.error());
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
    return impl_->call<std::uint64_t>(
        [this] { return Result<std::uint64_t>::success(impl_->admission.epoch()); });
}
Result<std::uint64_t> RuntimeInstance::prepare_idle_stop(std::uint64_t expected) {
    return impl_->call<std::uint64_t>(
        [this, expected] {
            IdleSnapshot idle;
            idle.duplicate_waiters = impl_->pending.size();
            for (const auto &[id, run] : impl_->runs) {
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
            for (const auto &[resource, envelope] : impl_->envelopes) {
                (void)envelope;
                auto ledger = impl_->resources.snapshot(resource);
                idle.nontransferable_cleanup += ledger.cleanup_records + ledger.allocations;
            }
            auto stopped = impl_->admission.prepare_idle_stop(expected, idle);
            if (stopped)
                impl_->accepting = false;
            return stopped;
        },
        true);
}
Result<void> RuntimeInstance::fence_admission() { return shutdown(); }
Result<void> RuntimeInstance::shutdown() {
    return impl_->call<void>(
        [this] {
            impl_->accepting = false;
            for (auto &[id, run] : impl_->runs) {
                (void)id;
                if (run->result.pending)
                    (void)run->controller->cancel_run();
            }
            return Result<void>::success();
        },
        true);
}
} // namespace flamoris::runtime
