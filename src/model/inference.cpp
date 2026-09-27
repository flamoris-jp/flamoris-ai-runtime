#include "flamoris/runtime/inference.hpp"
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>

namespace flamoris::runtime {
namespace {
Result<void> reject(ErrorCode code = ErrorCode::state_unavailable,
                    ErrorReason reason = ErrorReason::none) {
    return Result<void>::failure(ErrorEnvelope::make(code, ErrorStage::execution,
                                                     ExternalOutcome::not_applicable,
                                                     RetryDisposition::prohibited, reason));
}
bool cleanup_operation(NativeOperation operation) {
    return operation == NativeOperation::stop || operation == NativeOperation::release;
}
} // namespace
struct ThreadNativeWorker::Impl {
    NativeWorkerConfig config;
    ThreadNativeWorker::ComputeFactory compute_factory;
    std::unique_ptr<NativeSession> session;
    std::unique_ptr<ComputeImplementation> initializing_compute;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::optional<NativeCommand> request;
    std::optional<NativeObservation> response;
    bool busy{false}, closing{false}, closed{false}, released{false}, loaded{false};
    std::thread thread;
    std::size_t model_bytes{0};
    std::weak_ptr<const TinyModel> model_lifetime;
    explicit Impl(NativeWorkerConfig value, ThreadNativeWorker::ComputeFactory factory)
        : config(std::move(value)), compute_factory(std::move(factory)) {}
    NativeObservation perform(const NativeCommand &command) {
        NativeObservation observation;
        observation.ticket = command.ticket;
        observation.operation_generation = command.operation_generation;
        observation.operation = command.operation;
        const auto before_state =
            session ? std::optional<NativeExecutionState>(session->state()) : std::nullopt;
        auto error = [&](ErrorEnvelope value) { observation.error = value; };
        try {
            if (command.operation == NativeOperation::load) {
                if (session || loaded || initializing_compute)
                    error(ErrorEnvelope::make(ErrorCode::state_unavailable));
                else {
                    auto model =
                        config.shared_model
                            ? Result<std::shared_ptr<const TinyModel>>::success(
                                  std::move(config.shared_model))
                            : TinyModel::load(config.registered_artifact, config.artifact_sha256);
                    if (!model)
                        error(model.error());
                    else {
                        auto compute =
                            compute_factory
                                ? compute_factory(config)
                                : (config.opencl
                                       ? prepare_opencl_compute(config.device_index,
                                                                config.device_memory_bound)
                                       : ComputePreparation{make_cpu_compute(), {}});
                        initializing_compute = std::move(compute.owner);
                        if (compute.error)
                            error(*compute.error);
                        else if (!initializing_compute)
                            error(ErrorEnvelope::make(ErrorCode::native_compute_unavailable));
                        else if (initializing_compute->device().identity !=
                                     config.compute_identity ||
                                 model.value()->definition().artifact_sha256 !=
                                     config.artifact_sha256)
                            error(ErrorEnvelope::make(ErrorCode::plan_stale));
                        else {
                            model_bytes = model.value()->resident_bytes();
                            model_lifetime = model.value();
                            auto created = NativeSession::create_retaining_compute(
                                model.value(), initializing_compute, config.processor,
                                config.tokenizer, config.prompt, config.options);
                            if (!created)
                                error(created.error());
                            else {
                                session = std::move(created).value();
                                loaded = true;
                            }
                        }
                    }
                }
            } else if (!session) {
                if (command.operation == NativeOperation::release) {
                    auto close_result = initializing_compute ? initializing_compute->close()
                                                             : Result<void>::success();
                    if (!close_result)
                        error(close_result.error());
                    else {
                        const auto retained =
                            initializing_compute ? 131072 + config.retained_context_bytes : 0;
                        initializing_compute.reset();
                        observation.release = {0, retained, true};
                        observation.state.stage = NativeStage::released;
                        observation.state.valid = false;
                        observation.model_allocation_released = model_lifetime.expired();
                        released = true;
                    }
                } else if (command.operation == NativeOperation::stop && initializing_compute) {
                    auto stopped = initializing_compute->synchronize();
                    if (!stopped)
                        error(stopped.error());
                } else if (command.operation != NativeOperation::stop)
                    error(ErrorEnvelope::make(ErrorCode::state_unavailable));
            } else {
                switch (command.operation) {
                case NativeOperation::step: {
                    auto result = session->step(command.prefill_tokens);
                    if (!result)
                        error(result.error());
                    else
                        observation.segment = std::move(result).value();
                    break;
                }
                case NativeOperation::pause: {
                    auto result = session->pause();
                    if (!result)
                        error(result.error());
                    else
                        observation.suspension_generation = result.value();
                    break;
                }
                case NativeOperation::resume: {
                    auto result =
                        session->resume(command.resume_pins, command.suspension_generation);
                    if (!result)
                        error(result.error());
                    break;
                }
                case NativeOperation::inject: {
                    auto result = session->inject(command.input, command.injection_command);
                    if (!result)
                        error(result.error());
                    break;
                }
                case NativeOperation::stop:
                case NativeOperation::graceful_stop: {
                    auto result =
                        session->request_stop(command.operation == NativeOperation::graceful_stop);
                    if (!result)
                        error(result.error());
                    break;
                }
                case NativeOperation::release: {
                    auto result = session->release();
                    if (!result)
                        error(result.error());
                    else {
                        observation.release = result.value();
                        released = true;
                    }
                    break;
                }
                case NativeOperation::load:
                    break;
                }
            }
        } catch (const std::bad_alloc &) {
            error(ErrorEnvelope::make(ErrorCode::resource_unavailable, ErrorStage::execution));
        } catch (...) {
            error(ErrorEnvelope::make(ErrorCode::native_execution_failure, ErrorStage::execution));
        }
        if (session) {
            if (observation.error && before_state && before_state->valid &&
                session->state().version == before_state->version) {
                const auto code = observation.error->code();
                observation.control_rejected =
                    (command.operation == NativeOperation::inject &&
                     (code == ErrorCode::invalid_request || code == ErrorCode::budget_exceeded)) ||
                    (command.operation == NativeOperation::graceful_stop &&
                     code == ErrorCode::invalid_request) ||
                    (command.operation == NativeOperation::pause &&
                     (code == ErrorCode::state_unavailable ||
                      code == ErrorCode::budget_exceeded)) ||
                    (command.operation == NativeOperation::resume &&
                     code == ErrorCode::state_unavailable &&
                     command.resume_pins == before_state->pins);
            }
            if (observation.error && !observation.control_rejected) {
                // A failed segment never supplies a resumable continuation. Cleanup is
                // attempted on its owner; failed quiescence stays explicitly unresolved.
                auto stopped = session->request_stop();
                observation.quiescent = static_cast<bool>(stopped);
            } else
                observation.quiescent = true;
            observation.state = session->state();
            observation.model_bytes = model_bytes;
            if (command.operation == NativeOperation::load && !observation.error)
                observation.resident_model = session->model();
            observation.state_bytes =
                released ? 0 : session->reserved_state_bytes() + config.retained_context_bytes;
            observation.model_allocation_released = released && model_lifetime.expired();
        } else if (initializing_compute) {
            observation.state.valid = false;
            observation.state.stage = NativeStage::failed;
            observation.state_bytes = 131072 + config.retained_context_bytes;
            observation.quiescent = initializing_compute->receipt().quiescent;
        } else
            observation.quiescent = true;
        return observation;
    }
    void run() {
        for (;;) {
            NativeCommand command;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] { return closing || request.has_value(); });
                if (closing && !request)
                    break;
                command = std::move(*request);
                request.reset();
                busy = true;
            }
            auto observation = perform(command);
            {
                std::lock_guard lock(mutex);
                response.emplace(std::move(observation));
                busy = false;
            }
            condition.notify_all();
        }
        if (session && !released) {
            auto stopped = session->request_stop();
            if (stopped) {
                auto receipt = session->release();
                if (!receipt)
                    (void)session.release(); // Unsafe device residue must remain allocated.
            } else
                (void)session.release();
        }
        if (initializing_compute && !initializing_compute->close())
            (void)initializing_compute.release();
        std::lock_guard lock(mutex);
        closed = true;
    }
};
ThreadNativeWorker::ThreadNativeWorker(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {
    impl_->thread = std::thread([owner = impl_.get()] { owner->run(); });
}
Result<std::unique_ptr<ThreadNativeWorker>> ThreadNativeWorker::create(NativeWorkerConfig config,
                                                                       ComputeFactory factory) {
    if (config.prompt.size() > 255 || config.artifact_sha256.size() > 64 ||
        config.registered_artifact.native().size() > 4096 ||
        config.compute_identity.size() > 12288 || config.retained_context_bytes > 1073741824)
        return Result<std::unique_ptr<ThreadNativeWorker>>::failure(
            ErrorEnvelope::make(ErrorCode::invalid_request));
    try {
        return Result<std::unique_ptr<ThreadNativeWorker>>::success(
            std::unique_ptr<ThreadNativeWorker>(new ThreadNativeWorker(
                std::make_unique<Impl>(std::move(config), std::move(factory)))));
    } catch (...) {
        return Result<std::unique_ptr<ThreadNativeWorker>>::failure(
            ErrorEnvelope::make(ErrorCode::resource_unavailable));
    }
}
ThreadNativeWorker::~ThreadNativeWorker() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->closing = true;
    }
    impl_->condition.notify_all();
    if (impl_->thread.joinable())
        impl_->thread.join();
}
Result<void> ThreadNativeWorker::submit(NativeCommand command) {
    if (!command.operation_generation || command.input.size() > 255 || !command.prefill_tokens ||
        command.prefill_tokens > 32)
        return reject(ErrorCode::invalid_request);
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->busy || impl_->request || impl_->response)
        return reject(ErrorCode::resource_unavailable);
    impl_->request = std::move(command);
    impl_->condition.notify_one();
    return Result<void>::success();
}
std::optional<NativeObservation> ThreadNativeWorker::take() {
    std::lock_guard lock(impl_->mutex);
    auto result = std::move(impl_->response);
    impl_->response.reset();
    return result;
}
std::optional<NativeObservation> ThreadNativeWorker::wait_take(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    impl_->condition.wait_for(lock, timeout,
                              [&] { return impl_->response.has_value() || impl_->closed; });
    auto result = std::move(impl_->response);
    impl_->response.reset();
    return result;
}
bool ThreadNativeWorker::idle() const noexcept {
    std::lock_guard lock(impl_->mutex);
    return !impl_->busy && !impl_->request && !impl_->response;
}
Result<void> ThreadNativeWorker::close() {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->busy || impl_->request || impl_->response || impl_->initializing_compute ||
            (impl_->loaded && !impl_->released))
            return reject();
        impl_->closing = true;
    }
    impl_->condition.notify_all();
    if (impl_->thread.joinable())
        impl_->thread.join();
    return Result<void>::success();
}
InferenceMachine::InferenceMachine(std::unique_ptr<NativeWorkerPort> worker,
                                   std::uint64_t state_reference)
    : worker_(std::move(worker)), state_reference_(state_reference) {}
Result<void> InferenceMachine::submit(RunController &controller, DispatchTicket ticket,
                                      NativeOperation operation, DispatchChecks checks,
                                      NativeCommand command) {
    if (!worker_ || !state_reference_ || pending_)
        return reject();
    if (generation_ == std::numeric_limits<std::uint64_t>::max())
        return reject(ErrorCode::budget_exceeded);
    auto job = controller.job(ticket.job);
    if (!job || job.value().attempt != ticket.attempt ||
        job.value().dispatch_generation != ticket.dispatch_generation)
        return reject(ErrorCode::state_unavailable, ErrorReason::stale_observation);
    if (!cleanup_operation(operation)) {
        auto deadlines = controller.check_deadlines();
        if (!deadlines)
            return deadlines;
        job = controller.job(ticket.job);
        if (!job || job.value().state != JobState::running ||
            !controller.ticket_is_current(ticket) || !checks.authorized || !checks.pins_current ||
            !checks.state_valid || !checks.resources_granted)
            return reject(ErrorCode::permission_denied);
        if (job.value().pause_requested && operation != NativeOperation::pause)
            return reject(ErrorCode::state_unavailable);
    } else if (job.value().state != JobState::cancelling &&
               job.value().state != JobState::finalizing)
        return reject();
    command.ticket = ticket;
    command.operation_generation = generation_ + 1;
    command.operation = operation;
    auto submitted = worker_->submit(command);
    if (!submitted)
        return submitted;
    ++generation_;
    pending_ = std::move(command);
    return Result<void>::success();
}
std::optional<NativeObservation> InferenceMachine::take() {
    return worker_ ? worker_->take() : std::nullopt;
}
Result<void> InferenceMachine::accept(RunController &controller,
                                      const NativeObservation &observation) {
    if (!pending_ || pending_->operation_generation != observation.operation_generation ||
        pending_->ticket != observation.ticket || pending_->operation != observation.operation)
        return reject(ErrorCode::state_unavailable, ErrorReason::stale_observation);
    auto job = controller.job(observation.ticket.job);
    if (!job || job.value().attempt != observation.ticket.attempt ||
        job.value().dispatch_generation != observation.ticket.dispatch_generation)
        return reject(ErrorCode::state_unavailable, ErrorReason::stale_observation);
    std::optional<NativeExecutionState> prepared;
    try {
        prepared = observation.state;
    } catch (const std::bad_alloc &) {
        return reject(ErrorCode::resource_unavailable);
    }
    auto finish = [&](Result<void> result) {
        if (result) {
            state_ = std::move(prepared);
            pending_.reset();
        }
        return result;
    };
    if (observation.operation == NativeOperation::release) {
        if (job.value().state != JobState::finalizing ||
            (!observation.error && !observation.release.quiescent))
            return reject(ErrorCode::cleanup_failed);
        // Root reconciles physical allocation IDs before acknowledge_cleanup/finalize.
        return finish(Result<void>::success());
    }
    if (!controller.ticket_is_current(observation.ticket))
        return reject(ErrorCode::state_unavailable, ErrorReason::stale_observation);
    auto deadlines = controller.check_deadlines();
    if (!deadlines)
        return deadlines;
    job = controller.job(observation.ticket.job);
    if (!job)
        return reject();
    if (observation.control_rejected) {
        if (!observation.error || !observation.state.valid ||
            (state_ && (observation.state.pins != state_->pins ||
                        observation.state.version != state_->version)))
            return reject(ErrorCode::invariant_violation);
        // The receipt records a rejected request; it is not a failed inference Job.
        return finish(Result<void>::success());
    }
    if (!observation.error && state_ && observation.state.pins != state_->pins)
        return reject(ErrorCode::state_unavailable, ErrorReason::version_mismatch);
    if (!observation.error && observation.operation == NativeOperation::step &&
        (!observation.state.valid || !observation.segment.state_valid ||
         observation.segment.state_version != observation.state.version ||
         (state_ && observation.state.version <= state_->version)))
        return reject(ErrorCode::invariant_violation);
    if (observation.error) {
        if (job.value().state == JobState::running) {
            auto stop = controller.request_stop(observation.ticket.job, observation.error->code());
            if (!stop)
                return stop;
        }
        if (observation.quiescent)
            return finish(controller.observe_stopped(
                observation.ticket, {true, false, ExternalOutcome::not_applicable}));
        // A failed stop has completed its command, but has not proved quiescence.
        // Consume its receipt so the root can retry or retain cleanup debt.
        return finish(Result<void>::success());
    }
    if (job.value().state == JobState::cancelling) {
        if (observation.operation != NativeOperation::stop)
            return finish(Result<void>::success());
        if (observation.quiescent)
            return finish(controller.observe_stopped(
                observation.ticket, {true, false, ExternalOutcome::not_applicable}));
        return reject(ErrorCode::cleanup_timeout);
    }
    if (observation.operation == NativeOperation::pause) {
        ResumePayload payload;
        payload.state_reference = state_reference_;
        payload.state_version = observation.state.version;
        auto suspended = controller.suspend(
            observation.ticket, std::move(payload),
            {observation.quiescent, observation.state.valid && observation.state.paused});
        return finish(suspended ? Result<void>::success()
                                : Result<void>::failure(suspended.error()));
    }
    if ((observation.operation == NativeOperation::step && observation.segment.complete) ||
        observation.operation == NativeOperation::graceful_stop)
        return finish(controller.complete(observation.ticket));
    return finish(Result<void>::success());
}
Result<SpawnResult> InferenceMachine::accept_with_children(RunController &controller,
                                                           const NativeObservation &observation,
                                                           std::span<const ChildSpec> children) {
    auto denied = [] {
        return Result<SpawnResult>::failure(ErrorEnvelope::make(
            ErrorCode::state_unavailable, ErrorStage::execution, ExternalOutcome::not_applicable,
            RetryDisposition::prohibited, ErrorReason::stale_observation));
    };
    if (!pending_ || pending_->operation_generation != observation.operation_generation ||
        pending_->ticket != observation.ticket || pending_->operation != NativeOperation::pause ||
        observation.operation != NativeOperation::pause || observation.error ||
        observation.control_rejected || !observation.quiescent || !observation.state.valid ||
        !observation.state.paused || (state_ && observation.state.pins != state_->pins))
        return denied();
    auto deadlines = controller.check_deadlines();
    if (!deadlines)
        return Result<SpawnResult>::failure(deadlines.error());
    if (!controller.ticket_is_current(observation.ticket))
        return denied();
    std::optional<NativeExecutionState> prepared;
    try {
        prepared = observation.state;
    } catch (const std::bad_alloc &) {
        return Result<SpawnResult>::failure(ErrorEnvelope::make(ErrorCode::resource_unavailable));
    }
    ResumePayload payload;
    payload.state_reference = state_reference_;
    payload.state_version = observation.state.version;
    auto result = controller.spawn_and_suspend(observation.ticket, children, std::move(payload),
                                               {true, true});
    if (result) {
        state_ = std::move(prepared);
        pending_.reset();
    }
    return result;
}
Result<void> InferenceMachine::close() {
    return worker_ ? worker_->close() : Result<void>::success();
}
} // namespace flamoris::runtime
