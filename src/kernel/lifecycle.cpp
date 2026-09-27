#include "flamoris/runtime/lifecycle.hpp"
#include "flamoris/runtime/observation.hpp"
#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace flamoris::runtime {
namespace {
ErrorEnvelope error(ErrorCode code) { return ErrorEnvelope::make(code, ErrorStage::execution); }
Result<void> rejected(ErrorCode code = ErrorCode::invalid_request) {
    return Result<void>::failure(error(code));
}
LifecycleEvent event(std::string kind, JobId job, std::uint64_t generation = 0) {
    LifecycleEvent value;
    value.kind = std::move(kind);
    value.job = job;
    value.generation = generation;
    if (generation && value.kind.starts_with("interrupt."))
        value.command_id = CommandId{generation};
    return value;
}
LifecycleEvent changed(JobId job, JobState from, JobState to, std::optional<ErrorCode> code = {}) {
    auto value = event("job.state_changed", job);
    value.from = from;
    value.to = to;
    value.error = code;
    return value;
}
JobState stop_target(ErrorCode cause) {
    return cause == ErrorCode::job_cancelled || cause == ErrorCode::run_cancelled
               ? JobState::cancelled
               : JobState::failed;
}
} // namespace

bool is_terminal(JobState state) noexcept {
    return state == JobState::succeeded || state == JobState::failed ||
           state == JobState::cancelled;
}
const char *state_name(JobState state) noexcept {
    switch (state) {
    case JobState::created:
        return "created";
    case JobState::queued:
        return "queued";
    case JobState::running:
        return "running";
    case JobState::waiting:
        return "waiting";
    case JobState::paused:
        return "paused";
    case JobState::cancelling:
        return "cancelling";
    case JobState::finalizing:
        return "finalizing";
    case JobState::succeeded:
        return "succeeded";
    case JobState::failed:
        return "failed";
    case JobState::cancelled:
        return "cancelled";
    }
    return "invalid";
}
bool is_allowed_transition(JobState from, JobState to) noexcept {
    switch (from) {
    case JobState::created:
        return to == JobState::queued || to == JobState::cancelling;
    case JobState::queued:
        return to == JobState::running || to == JobState::paused || to == JobState::cancelling;
    case JobState::running:
        return to == JobState::waiting || to == JobState::paused || to == JobState::queued ||
               to == JobState::finalizing || to == JobState::cancelling;
    case JobState::waiting:
        return to == JobState::queued || to == JobState::paused || to == JobState::cancelling;
    case JobState::paused:
        return to == JobState::waiting || to == JobState::queued || to == JobState::cancelling;
    case JobState::cancelling:
        return to == JobState::finalizing;
    case JobState::finalizing:
        return is_terminal(to);
    default:
        return false;
    }
}

struct RunController::Impl {
    struct PauseCause {
        std::uint64_t id;
        bool barrier;
    };
    struct PauseReceipt {
        std::uint64_t id;
        JobId target;
        bool barrier;
        bool accepted;
    };
    struct Job {
        JobId id;
        std::optional<JobId> parent;
        JobState state{JobState::created};
        Deadline deadline;
        bool pause_supported{};
        bool in_flight{};
        bool cleanup_pending{};
        std::uint64_t attempt{};
        std::uint64_t dispatch_generation{};
        std::uint64_t suspension_generation{};
        std::uint64_t active_reference{};
        std::uint64_t active_version{};
        std::optional<ContinuationState> continuation;
        std::optional<PendingResume> pending;
        std::optional<JobState> intent;
        std::optional<ErrorCode> failure;
        ExternalOutcome external_outcome{ExternalOutcome::not_applicable};
        TimePoint cleanup_deadline{};
        bool cleanup_timed_out{};
        TimePoint not_before{};
        std::vector<PauseCause> pauses;
        Job(JobId value, std::optional<JobId> owner, Deadline end, bool pausable,
            std::size_t causes)
            : id(value), parent(owner), deadline(end), pause_supported(pausable) {
            pauses.reserve(causes);
        }
    };
    RunId id;
    LifecycleLimits limits;
    Deadline deadline;
    MonotonicClock &clock;
    std::vector<std::unique_ptr<Job>> jobs;
    std::vector<PauseReceipt> pause_receipts;
    EventStore store;
    std::uint64_t watermark{};
    std::uint64_t transition{};
    std::uint64_t next_job{2};
    std::size_t attempts{};
    std::size_t suspensions{};
    std::size_t commands{};
    bool dispatch_open{true};
    bool children_open{true};
    std::optional<std::size_t> preparation_failure_after;
    bool run_stopping{};
    bool healthy{true};
    std::uint64_t barrier_generation{};
    std::uint64_t barrier_command{};
    std::optional<Deadline> barrier_deadline;
    bool barrier_active{};
    bool barrier_pending{};

    Impl(RunId value, LifecycleLimits bound, Deadline end, MonotonicClock &time, bool pausable)
        : id(value), limits(bound), deadline(end), clock(time),
          store(ObservationLimits{bound.event_slots + 128,
                                  bound.event_slots - (32 + bound.jobs * 16), 32 + bound.jobs * 16,
                                  128, 0, 64 * 1024 * 1024, 16, 32}) {
        jobs.reserve(limits.jobs);
        pause_receipts.reserve(limits.commands);
        jobs.push_back(std::make_unique<Job>(JobId{id, 1}, std::nullopt, end, pausable,
                                             limits.pause_causes_per_job));
    }
    Job *find(JobId value) noexcept {
        for (auto &job : jobs)
            if (job->id == value)
                return job.get();
        return nullptr;
    }
    const Job *find(JobId value) const noexcept {
        for (const auto &job : jobs)
            if (job->id == value)
                return job.get();
        return nullptr;
    }
    bool descendant(const Job &job, JobId ancestor) const noexcept {
        const Job *current = &job;
        while (current) {
            if (current->id == ancestor)
                return true;
            current = current->parent ? find(*current->parent) : nullptr;
        }
        return false;
    }
    bool current(const DispatchTicket &ticket) const noexcept {
        auto *job = find(ticket.job);
        return job && job->attempt == ticket.attempt && ticket.attempt != 0 &&
               job->dispatch_generation == ticket.dispatch_generation &&
               (job->state == JobState::running || job->state == JobState::cancelling);
    }
    bool due(const Job &job) const noexcept { return job.deadline.expired(clock.now()); }
    bool mutable_work(const Job &job) const noexcept {
        return !is_terminal(job.state) && job.state != JobState::cancelling &&
               job.state != JobState::finalizing;
    }
    void refresh_barrier() noexcept {
        if (!barrier_active)
            return;
        barrier_pending = false;
        for (const auto &job : jobs)
            if (mutable_work(*job) && job->state != JobState::paused)
                barrier_pending = true;
    }
    RunActivity activity() const noexcept {
        const auto &root = *jobs.front();
        if (is_terminal(root.state)) {
            if (root.state == JobState::succeeded)
                return RunActivity::succeeded;
            if (root.state == JobState::failed)
                return RunActivity::failed;
            return RunActivity::cancelled;
        }
        if (root.state == JobState::finalizing)
            return RunActivity::finalizing;
        if (run_stopping || root.state == JobState::cancelling)
            return RunActivity::cancelling;
        bool queued = false, waiting = false, paused = false;
        for (const auto &job : jobs) {
            if (job->state == JobState::running)
                return RunActivity::running;
            queued |= job->state == JobState::queued;
            waiting |= job->state == JobState::waiting;
            paused |= job->state == JobState::paused;
        }
        if (queued)
            return RunActivity::queued;
        if (waiting || barrier_pending)
            return RunActivity::waiting;
        if (paused && barrier_active)
            return RunActivity::paused;
        if (root.state == JobState::created)
            return RunActivity::created;
        return RunActivity::waiting;
    }
    JobSnapshot snapshot(const Job &job) const {
        JobSnapshot view{job.id, job.parent, job.state, job.intent, job.failure, job.deadline};
        view.attempt = job.attempt;
        view.dispatch_generation = job.dispatch_generation;
        view.suspension_generation = job.suspension_generation;
        view.state_reference = job.active_reference;
        view.state_version = job.active_version;
        if (job.continuation) {
            view.state_reference = job.continuation->payload.state_reference;
            view.state_version = job.continuation->payload.state_version;
            view.wait_satisfied = job.continuation->payload.wait_satisfied;
        } else if (job.pending) {
            view.state_reference = job.pending->payload.state_reference;
            view.state_version = job.pending->payload.state_version;
            view.wait_satisfied = job.pending->payload.wait_satisfied;
        }
        view.continuation = job.continuation.has_value();
        view.pending_resume = job.pending.has_value();
        view.execution_in_flight = job.in_flight;
        view.cleanup_pending = job.cleanup_pending;
        view.pause_requested = !job.pauses.empty();
        view.pause_supported = job.pause_supported;
        view.pause_causes = job.pauses.size();
        return view;
    }
    template <class Mutation>
    Result<void> commit(std::vector<LifecycleEvent> events, Mutation mutation,
                        EventStorageClass storage = EventStorageClass::control,
                        std::uint64_t reservation = 0) {
        if (!healthy)
            return rejected(ErrorCode::invariant_violation);
        if (preparation_failure_after) {
            if (*preparation_failure_after == 0) {
                preparation_failure_after.reset();
                return rejected(ErrorCode::budget_exceeded);
            }
            --*preparation_failure_after;
        }
        const bool project_activity =
            storage != EventStorageClass::reconciliation && storage != EventStorageClass::telemetry;
        if (project_activity) {
            auto projected = event("run.state_changed", jobs.front()->id);
            projected.run_from = activity();
            projected.run_to = activity();
            events.push_back(std::move(projected));
        }
        if (events.empty() ||
            events.size() > std::numeric_limits<std::uint64_t>::max() - watermark ||
            transition == std::numeric_limits<std::uint64_t>::max())
            return rejected(ErrorCode::budget_exceeded);
        EventGroup group{transition + 1, watermark + 1, std::move(events)};
        std::uint64_t next = watermark;
        for (auto &value : group.events) {
            value.sequence = ++next;
            value.monotonic_offset = static_cast<std::uint64_t>(clock.now().count());
            if (const auto *owner = find(value.job); owner && value.kind != "run.state_changed") {
                if (!value.attempt_id && owner->attempt)
                    value.attempt_id = AttemptId{owner->attempt};
                if (!value.dispatch_generation && owner->dispatch_generation)
                    value.dispatch_generation = DispatchGeneration{owner->dispatch_generation};
                if (!value.external_outcome &&
                    owner->external_outcome != ExternalOutcome::not_applicable)
                    value.external_outcome = owner->external_outcome;
            }
        }
        auto prepared = store.prepare(std::move(group), storage, reservation);
        if (!prepared)
            return Result<void>::failure(prepared.error());
        // All storage is reserved at admission or owned by the prepared group.
        // There are no callbacks, allocation, serialization or observers here.
        mutation();
        refresh_barrier();
        if (project_activity)
            prepared.value().set_run_activity(activity());
        if (!store.publish(std::move(prepared).value())) {
            healthy = false;
            dispatch_open = false;
            children_open = false;
            return rejected(ErrorCode::invariant_violation);
        }
        watermark = next;
        ++transition;
        return Result<void>::success();
    }
    void clear_payload(Job &job) noexcept {
        if (job.continuation) {
            job.active_reference = job.continuation->payload.state_reference;
            job.active_version = job.continuation->payload.state_version;
        }
        if (job.pending) {
            job.active_reference = job.pending->payload.state_reference;
            job.active_version = job.pending->payload.state_version;
        }
        job.continuation.reset();
        job.pending.reset();
        job.pauses.clear();
    }
    void freeze(Job &job) noexcept {
        job.state = JobState::finalizing;
        job.intent = job.failure ? stop_target(*job.failure) : JobState::succeeded;
        if (job.cleanup_deadline == TimePoint{})
            set_cleanup_deadline(job);
        if (job.id == jobs.front()->id) {
            dispatch_open = false;
            children_open = false;
        }
    }
    void set_cleanup_deadline(Job &job) noexcept {
        const auto remaining = std::numeric_limits<TimePoint::rep>::max() - clock.now().count();
        job.cleanup_deadline = remaining < limits.cleanup_allowance.count()
                                   ? TimePoint::max()
                                   : clock.now() + limits.cleanup_allowance;
    }
};

RunController::RunController(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RunController::~RunController() = default;
Result<std::unique_ptr<RunController>> RunController::create(RunId id, LifecycleLimits limits,
                                                             Deadline deadline,
                                                             MonotonicClock &clock, bool pausable) {
    if (!id.valid() || clock.now().count() < 0 || limits.jobs == 0 || limits.jobs > 65536 ||
        limits.attempts == 0 || limits.pause_causes_per_job == 0 ||
        limits.cleanup_allowance.count() <= 0 || limits.event_slots < 34 + limits.jobs * 16 ||
        limits.event_slots > 8192 || deadline.expired(clock.now()))
        return Result<std::unique_ptr<RunController>>::failure(error(ErrorCode::budget_exceeded));
    try {
        auto owner = std::unique_ptr<RunController>(
            new RunController(std::make_unique<Impl>(id, limits, deadline, clock, pausable)));
        auto result = owner->impl_->commit(
            {event("run.created", owner->root()), event("job.created", owner->root())},
            []() noexcept {});
        if (!result)
            return Result<std::unique_ptr<RunController>>::failure(result.error());
        return Result<std::unique_ptr<RunController>>::success(std::move(owner));
    } catch (const std::bad_alloc &) {
        return Result<std::unique_ptr<RunController>>::failure(error(ErrorCode::budget_exceeded));
    } catch (const std::invalid_argument &) {
        return Result<std::unique_ptr<RunController>>::failure(error(ErrorCode::invalid_request));
    }
}
JobId RunController::root() const noexcept { return impl_->jobs.front()->id; }
RunId RunController::id() const noexcept { return impl_->id; }
RunSnapshot RunController::snapshot() const {
    RunSnapshot view{id(),
                     root(),
                     impl_->activity(),
                     impl_->watermark,
                     impl_->dispatch_open,
                     impl_->children_open,
                     impl_->barrier_generation,
                     impl_->barrier_pending,
                     {}};
    view.jobs.reserve(impl_->jobs.size());
    for (const auto &job : impl_->jobs)
        view.jobs.push_back(impl_->snapshot(*job));
    return view;
}
Result<JobSnapshot> RunController::job(JobId id) const {
    auto *found = impl_->find(id);
    if (!found)
        return Result<JobSnapshot>::failure(error(ErrorCode::invalid_request));
    return Result<JobSnapshot>::success(impl_->snapshot(*found));
}
const std::vector<EventGroup> &RunController::events() const noexcept {
    return impl_->store.groups();
}
EventStore &RunController::observation_store() noexcept { return impl_->store; }
Result<void> RunController::commit_observation(EventGroup group, EventStorageClass storage,
                                               std::uint64_t reservation) {
    for (const auto &value : group.events) {
        if (value.job.run() != id() ||
            (value.kind != "reconciliation.observed" && value.kind != "reconciliation.closed" &&
             value.kind != "token.generated" && value.kind != "inference.stage_changed" &&
             value.kind != "race.winner_selected" && value.kind != "group.completed"))
            return rejected();
    }
    return impl_->commit(std::move(group.events), []() noexcept {}, storage, reservation);
}
bool RunController::ticket_is_current(const DispatchTicket &ticket) const noexcept {
    return impl_->current(ticket);
}
void RunController::fail_next_preparation() noexcept { fail_preparation_after(0); }
void RunController::fail_preparation_after(std::size_t successful_preparations) noexcept {
    impl_->preparation_failure_after = successful_preparations;
}

Result<JobId> RunController::register_child(JobId parent, Deadline deadline, bool pausable) {
    auto *owner = impl_->find(parent);
    if (!owner || !impl_->mutable_work(*owner) || !impl_->children_open ||
        deadline.time() > owner->deadline.time() || deadline.expired(impl_->clock.now()))
        return Result<JobId>::failure(error(ErrorCode::invalid_request));
    if (impl_->jobs.size() == impl_->limits.jobs ||
        impl_->next_job == std::numeric_limits<std::uint64_t>::max())
        return Result<JobId>::failure(error(ErrorCode::budget_exceeded));
    JobId id{impl_->id, impl_->next_job};
    auto child = std::make_unique<Impl::Job>(id, parent, deadline, pausable,
                                             impl_->limits.pause_causes_per_job);
    auto result = impl_->commit({event("job.created", id)}, [&]() noexcept {
        impl_->jobs.push_back(std::move(child));
        ++impl_->next_job;
    });
    if (!result)
        return Result<JobId>::failure(result.error());
    return Result<JobId>::success(id);
}
Result<void> RunController::queue(JobId id) {
    auto *job = impl_->find(id);
    if (!job || job->state != JobState::created || !impl_->dispatch_open)
        return rejected();
    if (impl_->due(*job))
        return request_stop(id, ErrorCode::job_timeout);
    return impl_->commit({changed(id, job->state, JobState::queued)},
                         [&]() noexcept { job->state = JobState::queued; });
}
Result<DispatchTicket> RunController::dispatch(JobId id, DispatchChecks checks) {
    auto *job = impl_->find(id);
    auto deny = [](ErrorCode code) { return Result<DispatchTicket>::failure(error(code)); };
    if (!job || job->state != JobState::queued || !impl_->dispatch_open || !job->pauses.empty())
        return deny(ErrorCode::invalid_request);
    if (impl_->due(*job)) {
        auto result = request_stop(id, ErrorCode::job_timeout);
        (void)result;
        return deny(ErrorCode::job_timeout);
    }
    if (!checks.authorized || !checks.pins_current || !checks.state_valid) {
        auto code = !checks.authorized ? ErrorCode::permission_denied
                                       : (!checks.pins_current ? ErrorCode::plan_stale
                                                               : ErrorCode::state_unavailable);
        auto result = request_stop(id, code);
        (void)result;
        return deny(code);
    }
    if (!checks.resources_granted || impl_->clock.now() < job->not_before)
        return deny(ErrorCode::resource_unavailable);
    if ((job->attempt == 0 && impl_->attempts >= impl_->limits.attempts) ||
        job->dispatch_generation == std::numeric_limits<std::uint64_t>::max())
        return deny(ErrorCode::budget_exceeded);
    const auto attempt = job->attempt == 0 ? 1 : job->attempt;
    DispatchTicket ticket{id, attempt, job->dispatch_generation + 1};
    std::vector<LifecycleEvent> events{
        event("resource.granted", id, ticket.dispatch_generation),
        changed(id, JobState::queued, JobState::running),
        event("attempt.dispatch_committed", id, ticket.dispatch_generation)};
    for (auto &value : events) {
        value.attempt_id = AttemptId{ticket.attempt};
        value.dispatch_generation = DispatchGeneration{ticket.dispatch_generation};
    }
    auto result = impl_->commit(std::move(events), [&]() noexcept {
        if (job->attempt == 0)
            ++impl_->attempts;
        job->attempt = attempt;
        ++job->dispatch_generation;
        if (job->pending) {
            job->active_reference = job->pending->payload.state_reference;
            job->active_version = job->pending->payload.state_version;
            job->pending.reset();
        }
        job->state = JobState::running;
        job->in_flight = true;
        job->cleanup_pending = true;
    });
    if (!result)
        return Result<DispatchTicket>::failure(result.error());
    return Result<DispatchTicket>::success(ticket);
}
Result<std::uint64_t> RunController::suspend(DispatchTicket ticket, ResumePayload payload,
                                             SafePointEvidence evidence) {
    auto deny = [](ErrorCode code) { return Result<std::uint64_t>::failure(error(code)); };
    auto *job = impl_->find(ticket.job);
    if (!impl_->current(ticket) || !job || job->state != JobState::running || !evidence.quiescent ||
        !evidence.preserved || !job->pause_supported)
        return deny(ErrorCode::state_unavailable);
    if (impl_->due(*job)) {
        auto result = request_stop(job->id, ErrorCode::job_timeout);
        (void)result;
        return deny(ErrorCode::job_timeout);
    }
    if (impl_->suspensions == impl_->limits.suspensions ||
        job->suspension_generation == std::numeric_limits<std::uint64_t>::max() ||
        payload.wait_set.size() > impl_->limits.jobs)
        return deny(ErrorCode::budget_exceeded);
    for (std::size_t i = 0; i < payload.wait_set.size(); ++i) {
        auto *child = impl_->find(payload.wait_set[i]);
        if (!child || child->parent != job->id)
            return deny(ErrorCode::invalid_request);
        for (std::size_t j = 0; j < i; ++j)
            if (payload.wait_set[i] == payload.wait_set[j])
                return deny(ErrorCode::invalid_request);
    }
    if (payload.wait_set.empty() && !payload.wait_satisfied)
        return deny(ErrorCode::invalid_request);
    const auto generation = job->suspension_generation + 1;
    ContinuationState continuation{job->id, generation, std::move(payload)};
    const auto target = job->pauses.empty() ? JobState::waiting : JobState::paused;
    std::vector<LifecycleEvent> events{
        event("continuation.created", job->id, generation), changed(job->id, job->state, target),
        event("resource.execution_quiesced", job->id, ticket.dispatch_generation)};
    for (const auto &cause : job->pauses)
        if (!cause.barrier)
            events.push_back(event("interrupt.applied", job->id, cause.id));
    bool barrier_complete = impl_->barrier_active;
    for (const auto &peer : impl_->jobs)
        if (peer.get() != job && impl_->mutable_work(*peer) && peer->state != JobState::paused)
            barrier_complete = false;
    if (barrier_complete)
        events.push_back(event("interrupt.applied", root(), impl_->barrier_command));
    auto result = impl_->commit(std::move(events), [&]() noexcept {
        job->continuation.emplace(std::move(continuation));
        job->active_reference = 0;
        job->active_version = 0;
        job->state = target;
        job->in_flight = false;
        ++job->suspension_generation;
        ++impl_->suspensions;
    });
    if (!result)
        return Result<std::uint64_t>::failure(result.error());
    return Result<std::uint64_t>::success(generation);
}
Result<void> RunController::wake(JobId id, std::uint64_t generation) {
    auto *job = impl_->find(id);
    if (!job || !job->continuation || job->continuation->suspension_generation != generation ||
        (job->state != JobState::waiting && job->state != JobState::paused))
        return rejected();
    if (impl_->due(*job))
        return request_stop(id, ErrorCode::job_timeout);
    if (job->state == JobState::paused) {
        if (job->continuation->payload.wait_satisfied)
            return Result<void>::success();
        return impl_->commit({event("continuation.condition_satisfied", id, generation)},
                             [&]() noexcept { job->continuation->payload.wait_satisfied = true; });
    }
    if (!impl_->dispatch_open)
        return rejected();
    return impl_->commit(
        {event("continuation.consumed", id, generation), changed(id, job->state, JobState::queued)},
        [&]() noexcept {
            job->continuation->payload.wait_satisfied = true;
            job->pending.emplace(generation, std::move(job->continuation->payload));
            job->continuation.reset();
            job->state = JobState::queued;
        });
}

Result<SpawnResult> RunController::spawn_and_suspend(DispatchTicket ticket,
                                                     std::span<const ChildSpec> specs,
                                                     ResumePayload payload,
                                                     SafePointEvidence evidence) {
    auto deny = [](ErrorCode code) { return Result<SpawnResult>::failure(error(code)); };
    auto *parent = impl_->find(ticket.job);
    if (!impl_->current(ticket) || !parent || parent->state != JobState::running ||
        !impl_->children_open || !parent->pauses.empty() || !evidence.quiescent ||
        !evidence.preserved || !parent->pause_supported || specs.empty() ||
        !payload.wait_set.empty())
        return deny(ErrorCode::invalid_request);
    if (impl_->due(*parent)) {
        auto stopped = request_stop(parent->id, ErrorCode::job_timeout);
        (void)stopped;
        return deny(ErrorCode::job_timeout);
    }
    if (specs.size() > impl_->limits.jobs - impl_->jobs.size() ||
        impl_->suspensions >= impl_->limits.suspensions ||
        specs.size() >= std::numeric_limits<std::uint64_t>::max() - impl_->next_job ||
        parent->suspension_generation == std::numeric_limits<std::uint64_t>::max())
        return deny(ErrorCode::budget_exceeded);
    SpawnResult result;
    result.suspension_generation = parent->suspension_generation + 1;
    result.children.reserve(specs.size());
    payload.wait_set.reserve(specs.size());
    std::vector<std::unique_ptr<Impl::Job>> children;
    children.reserve(specs.size());
    std::vector<LifecycleEvent> events;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        if (specs[i].deadline.time() > parent->deadline.time() ||
            specs[i].deadline.expired(impl_->clock.now()))
            return deny(ErrorCode::invalid_request);
        JobId id{impl_->id, impl_->next_job + i};
        children.push_back(std::make_unique<Impl::Job>(id, parent->id, specs[i].deadline,
                                                       specs[i].pause_supported,
                                                       impl_->limits.pause_causes_per_job));
        result.children.push_back(id);
        payload.wait_set.push_back(id);
    }
    payload.wait_satisfied = false;
    ContinuationState continuation{parent->id, result.suspension_generation, std::move(payload)};
    events.push_back(event("continuation.created", parent->id, result.suspension_generation));
    events.push_back(changed(parent->id, parent->state, JobState::waiting));
    for (auto id : result.children)
        events.push_back(event("job.created", id));
    events.push_back(event("resource.execution_quiesced", parent->id, ticket.dispatch_generation));
    auto committed = impl_->commit(std::move(events), [&]() noexcept {
        parent->continuation.emplace(std::move(continuation));
        parent->state = JobState::waiting;
        parent->in_flight = false;
        parent->active_reference = 0;
        parent->active_version = 0;
        ++parent->suspension_generation;
        ++impl_->suspensions;
        impl_->next_job += children.size();
        for (auto &child : children)
            impl_->jobs.push_back(std::move(child));
    });
    if (!committed)
        return Result<SpawnResult>::failure(committed.error());
    return Result<SpawnResult>::success(std::move(result));
}

Result<void> RunController::pause_job(JobId id, std::uint64_t command) {
    auto *job = impl_->find(id);
    if (!job || command == 0)
        return rejected();
    for (const auto &receipt : impl_->pause_receipts)
        if (receipt.id == command) {
            if (receipt.barrier || receipt.target != id)
                return rejected();
            return receipt.accepted ? Result<void>::success()
                                    : rejected(ErrorCode::capability_unavailable);
        }
    for (const auto &cause : job->pauses)
        if (!cause.barrier && cause.id == command)
            return Result<void>::success();
    if (impl_->commands >= impl_->limits.commands)
        return rejected(ErrorCode::budget_exceeded);
    if (!impl_->mutable_work(*job) || job->state == JobState::created ||
        (job->state == JobState::running && !job->pause_supported) ||
        job->pauses.size() == impl_->limits.pause_causes_per_job) {
        auto result = impl_->commit(
            {event("interrupt.requested", id, command), event("interrupt.rejected", id, command)},
            [&]() noexcept {
                ++impl_->commands;
                impl_->pause_receipts.push_back({command, id, false, false});
            });
        return result ? rejected(ErrorCode::capability_unavailable) : result;
    }
    if (impl_->due(*job))
        return request_stop(id, ErrorCode::job_timeout);
    std::vector<LifecycleEvent> events{event("interrupt.requested", id, command)};
    const auto queued = job->state == JobState::queued;
    if (queued && impl_->suspensions >= impl_->limits.suspensions)
        return rejected(ErrorCode::budget_exceeded);
    if (queued)
        events.push_back(event("continuation.created", id, job->suspension_generation + 1));
    if (job->state != JobState::running && job->state != JobState::paused)
        events.push_back(changed(id, job->state, JobState::paused));
    if (job->state != JobState::running)
        events.push_back(event("interrupt.applied", id, command));
    return impl_->commit(std::move(events), [&]() noexcept {
        ++impl_->commands;
        impl_->pause_receipts.push_back({command, id, false, true});
        job->pauses.push_back({command, false});
        if (queued) {
            ResumePayload payload;
            if (job->pending) {
                payload = std::move(job->pending->payload);
                job->pending.reset();
            }
            job->continuation.emplace(id, ++job->suspension_generation, std::move(payload));
            ++impl_->suspensions;
        }
        if (job->state != JobState::running)
            job->state = JobState::paused;
    });
}
Result<void> RunController::resume_job(JobId id, std::uint64_t command, bool authorized) {
    auto *job = impl_->find(id);
    if (!job)
        return rejected();
    auto cause = std::find_if(job->pauses.begin(), job->pauses.end(),
                              [&](auto value) { return !value.barrier && value.id == command; });
    if (cause == job->pauses.end())
        return impl_->mutable_work(*job) ? Result<void>::success() : rejected();
    if (!authorized)
        return rejected(ErrorCode::permission_denied);
    if (impl_->due(*job))
        return request_stop(id, ErrorCode::job_timeout);
    if (impl_->commands >= impl_->limits.commands)
        return rejected(ErrorCode::budget_exceeded);
    const bool last = job->pauses.size() == 1;
    const auto target =
        last && job->state == JobState::paused
            ? (job->continuation->payload.wait_satisfied ? JobState::queued : JobState::waiting)
            : job->state;
    std::vector<LifecycleEvent> events{event("interrupt.requested", id, command)};
    if (target == JobState::queued)
        events.push_back(event("continuation.consumed", id, job->suspension_generation));
    if (target != job->state)
        events.push_back(changed(id, job->state, target));
    events.push_back(event("interrupt.applied", id, command));
    return impl_->commit(std::move(events), [&]() noexcept {
        ++impl_->commands;
        job->pauses.erase(cause);
        if (target == JobState::queued) {
            job->pending.emplace(job->suspension_generation, std::move(job->continuation->payload));
            job->continuation.reset();
        }
        job->state = target;
    });
}

Result<void> RunController::pause_run(std::uint64_t command, Deadline request_deadline) {
    for (const auto &receipt : impl_->pause_receipts)
        if (receipt.id == command) {
            if (!receipt.barrier)
                return rejected();
            return receipt.accepted ? Result<void>::success()
                                    : rejected(ErrorCode::capability_unavailable);
        }
    if (command == 0 || request_deadline.expired(impl_->clock.now()) || impl_->run_stopping ||
        !impl_->dispatch_open)
        return rejected();
    if (impl_->barrier_active)
        return impl_->barrier_command == command ? Result<void>::success() : rejected();
    if (impl_->commands >= impl_->limits.commands ||
        impl_->barrier_generation == std::numeric_limits<std::uint64_t>::max())
        return rejected(ErrorCode::budget_exceeded);
    std::size_t new_suspensions = 0;
    for (const auto &job : impl_->jobs) {
        if (!impl_->mutable_work(*job))
            continue;
        if ((job->state == JobState::running && !job->pause_supported) ||
            job->state == JobState::created ||
            job->pauses.size() == impl_->limits.pause_causes_per_job) {
            auto result =
                impl_->commit({event("interrupt.requested", root(), command),
                               event("interrupt.rejected", root(), command)},
                              [&]() noexcept {
                                  ++impl_->commands;
                                  impl_->pause_receipts.push_back({command, root(), true, false});
                              });
            return result ? rejected(ErrorCode::capability_unavailable) : result;
        }
        if (impl_->due(*job))
            return request_stop(job->id, ErrorCode::job_timeout);
        if (job->state == JobState::queued)
            ++new_suspensions;
    }
    if (new_suspensions > impl_->limits.suspensions - impl_->suspensions)
        return rejected(ErrorCode::budget_exceeded);
    std::vector<LifecycleEvent> events{
        event("interrupt.requested", root(), command),
        event("run.pause_barrier_created", root(), impl_->barrier_generation + 1)};
    for (const auto &job : impl_->jobs) {
        if (!impl_->mutable_work(*job))
            continue;
        if (job->state == JobState::queued)
            events.push_back(
                event("continuation.created", job->id, job->suspension_generation + 1));
        if (job->state == JobState::queued || job->state == JobState::waiting)
            events.push_back(changed(job->id, job->state, JobState::paused));
    }
    if (std::none_of(impl_->jobs.begin(), impl_->jobs.end(),
                     [](const auto &job) { return job->state == JobState::running; }))
        events.push_back(event("interrupt.applied", root(), command));
    return impl_->commit(std::move(events), [&]() noexcept {
        ++impl_->commands;
        impl_->pause_receipts.push_back({command, root(), true, true});
        ++impl_->barrier_generation;
        impl_->barrier_command = command;
        impl_->barrier_active = true;
        impl_->barrier_pending = true;
        impl_->barrier_deadline = request_deadline;
        impl_->dispatch_open = false;
        impl_->children_open = false;
        for (auto &job : impl_->jobs) {
            if (!impl_->mutable_work(*job))
                continue;
            job->pauses.push_back({impl_->barrier_generation, true});
            if (job->state == JobState::queued) {
                ResumePayload payload;
                if (job->pending) {
                    payload = std::move(job->pending->payload);
                    job->pending.reset();
                }
                job->continuation.emplace(job->id, ++job->suspension_generation,
                                          std::move(payload));
                ++impl_->suspensions;
            }
            if (job->state == JobState::queued || job->state == JobState::waiting)
                job->state = JobState::paused;
        }
    });
}
Result<void> RunController::resume_run(std::uint64_t command, bool authorized) {
    if (!authorized)
        return rejected(ErrorCode::permission_denied);
    if (impl_->run_stopping || !impl_->mutable_work(*impl_->jobs.front()))
        return rejected();
    if (!impl_->barrier_active)
        return Result<void>::success();
    if (impl_->deadline.expired(impl_->clock.now()))
        return cancel_run(ErrorCode::run_timeout);
    const bool timeout = command == impl_->barrier_command && impl_->barrier_pending &&
                         impl_->barrier_deadline &&
                         impl_->barrier_deadline->expired(impl_->clock.now());
    if (!timeout && impl_->commands >= impl_->limits.commands)
        return rejected(ErrorCode::budget_exceeded);
    std::vector<LifecycleEvent> events;
    if (!timeout)
        events.push_back(event("interrupt.requested", root(), command));
    for (const auto &job : impl_->jobs) {
        bool barrier = std::any_of(job->pauses.begin(), job->pauses.end(),
                                   [](auto cause) { return cause.barrier; });
        if (!barrier || job->pauses.size() != 1 || job->state != JobState::paused)
            continue;
        auto target =
            job->continuation->payload.wait_satisfied ? JobState::queued : JobState::waiting;
        if (target == JobState::queued)
            events.push_back(event("continuation.consumed", job->id, job->suspension_generation));
        events.push_back(changed(job->id, job->state, target));
    }
    events.push_back(event(timeout ? "interrupt.rejected" : "interrupt.applied", root(), command));
    return impl_->commit(
        std::move(events),
        [&]() noexcept {
            if (!timeout)
                ++impl_->commands;
            impl_->barrier_active = false;
            impl_->barrier_pending = false;
            impl_->barrier_deadline.reset();
            impl_->dispatch_open = true;
            impl_->children_open = true;
            for (auto &job : impl_->jobs) {
                std::erase_if(job->pauses, [](auto cause) { return cause.barrier; });
                if (job->state != JobState::paused || !job->pauses.empty())
                    continue;
                if (job->continuation->payload.wait_satisfied) {
                    job->pending.emplace(job->suspension_generation,
                                         std::move(job->continuation->payload));
                    job->continuation.reset();
                    job->state = JobState::queued;
                } else
                    job->state = JobState::waiting;
            }
        },
        timeout ? EventStorageClass::emergency : EventStorageClass::control);
}
Result<void> RunController::expire_pause_barrier() {
    if (!impl_->barrier_active || !impl_->barrier_pending || !impl_->barrier_deadline ||
        !impl_->barrier_deadline->expired(impl_->clock.now()))
        return Result<void>::success();
    // Deadline expiry removes only this Run's causes through the ordinary transfer
    // path. Job workload deadlines are never changed by this command.
    return resume_run(impl_->barrier_command, true);
}

Result<void> RunController::complete(DispatchTicket ticket, ExternalOutcome outcome) {
    auto *job = impl_->find(ticket.job);
    if (!impl_->current(ticket) || !job || job->state != JobState::running)
        return rejected();
    if (impl_->due(*job))
        return request_stop(job->id, ErrorCode::job_timeout);
    auto observed = event("attempt.outcome", job->id, ticket.dispatch_generation);
    observed.external_outcome = outcome;
    auto transition = changed(job->id, job->state, JobState::finalizing);
    transition.external_outcome = outcome;
    return impl_->commit({std::move(observed), std::move(transition)}, [&]() noexcept {
        job->in_flight = false;
        job->external_outcome = outcome;
        impl_->clear_payload(*job);
        impl_->freeze(*job);
    });
}
Result<void> RunController::request_stop(JobId id, ErrorCode cause) {
    auto *owner = impl_->find(id);
    if (!owner)
        return rejected();
    if (!impl_->mutable_work(*owner))
        return Result<void>::success();
    std::vector<LifecycleEvent> events;
    for (const auto &job : impl_->jobs) {
        if (!impl_->descendant(*job, id) || !impl_->mutable_work(*job))
            continue;
        events.push_back(changed(job->id, job->state, JobState::cancelling, cause));
        if (job->continuation || job->pending)
            events.push_back(event("continuation.discarded", job->id, job->suspension_generation));
        if (!job->in_flight)
            events.push_back(changed(job->id, JobState::cancelling, JobState::finalizing, cause));
    }
    return impl_->commit(
        std::move(events),
        [&]() noexcept {
            for (auto &job : impl_->jobs) {
                if (!impl_->descendant(*job, id) || !impl_->mutable_work(*job))
                    continue;
                job->failure = cause;
                job->state = JobState::cancelling;
                impl_->set_cleanup_deadline(*job);
                impl_->clear_payload(*job);
                if (!job->in_flight)
                    impl_->freeze(*job);
            }
            if (id == root()) {
                impl_->run_stopping = true;
                impl_->dispatch_open = false;
                impl_->children_open = false;
            }
        },
        EventStorageClass::emergency);
}
Result<void> RunController::cancel_run(ErrorCode cause) { return request_stop(root(), cause); }
Result<void> RunController::observe_stopped(DispatchTicket ticket, StopEvidence evidence) {
    auto *job = impl_->find(ticket.job);
    if (!impl_->current(ticket) || !job || job->state != JobState::cancelling)
        return rejected();
    if (!evidence.quiescent && !evidence.safely_contained)
        return rejected(ErrorCode::cleanup_timeout);
    auto cause = job->failure.value_or(ErrorCode::job_cancelled);
    if (evidence.external_outcome == ExternalOutcome::unknown &&
        stop_target(cause) == JobState::cancelled)
        cause = ErrorCode::outcome_unknown;
    auto observed = event("execution.stopped", job->id, ticket.dispatch_generation);
    observed.external_outcome = evidence.external_outcome;
    auto transition = changed(job->id, job->state, JobState::finalizing, cause);
    transition.external_outcome = evidence.external_outcome;
    return impl_->commit(
        {std::move(observed), std::move(transition)},
        [&]() noexcept {
            job->in_flight = false;
            job->failure = cause;
            job->external_outcome = evidence.external_outcome;
            impl_->freeze(*job);
        },
        EventStorageClass::emergency);
}
Result<void> RunController::acknowledge_cleanup(JobId id, CleanupEvidence evidence) {
    auto *job = impl_->find(id);
    if (!job || job->state != JobState::finalizing || job->in_flight)
        return rejected();
    if (!job->cleanup_pending)
        return Result<void>::success();
    if (!evidence.released &&
        !(evidence.transferred && evidence.safely_contained && evidence.closure_reserved))
        return rejected(ErrorCode::cleanup_failed);
    return impl_->commit(
        {event(evidence.released ? "resource.release_confirmed" : "cleanup.transferred", id)},
        [&]() noexcept {
            job->cleanup_pending = false;
            job->active_reference = 0;
            job->active_version = 0;
        },
        EventStorageClass::emergency);
}
Result<void> RunController::finalize(JobId id) {
    auto *job = impl_->find(id);
    if (!job || job->state != JobState::finalizing || job->cleanup_pending || job->in_flight ||
        !job->intent)
        return rejected();
    for (const auto &child : impl_->jobs)
        if (child->parent == id && !is_terminal(child->state))
            return rejected();
    std::vector<LifecycleEvent> events{changed(id, job->state, *job->intent, job->failure)};
    if (id == root())
        events.push_back(event("run.terminal", id));
    return impl_->commit(
        std::move(events), [&]() noexcept { job->state = *job->intent; },
        EventStorageClass::emergency);
}
Result<void> RunController::check_deadlines() {
    if (impl_->mutable_work(*impl_->jobs.front()) && impl_->deadline.expired(impl_->clock.now()))
        return cancel_run(ErrorCode::run_timeout);
    for (const auto &job : impl_->jobs)
        if (impl_->mutable_work(*job) && impl_->due(*job)) {
            auto result = request_stop(job->id, ErrorCode::job_timeout);
            if (!result)
                return result;
        }
    for (const auto &job : impl_->jobs) {
        if ((job->state != JobState::cancelling && job->state != JobState::finalizing) ||
            !job->cleanup_pending || job->cleanup_timed_out ||
            impl_->clock.now() < job->cleanup_deadline)
            continue;
        auto pending = event("cleanup.pending", job->id);
        pending.error = ErrorCode::cleanup_timeout;
        auto result = impl_->commit(
            {std::move(pending)}, [&]() noexcept { job->cleanup_timed_out = true; },
            EventStorageClass::emergency);
        if (!result)
            return result;
    }
    return expire_pause_barrier();
}
Result<void> RunController::retry(DispatchTicket ticket, TimePoint not_before, bool authorized,
                                  bool stopped, bool reconciled, ExternalOutcome previous_outcome) {
    auto *job = impl_->find(ticket.job);
    if (!impl_->current(ticket) || !job || job->state != JobState::running || !authorized ||
        !stopped || !reconciled || !job->pauses.empty() || !impl_->dispatch_open ||
        not_before >= job->deadline.time())
        return rejected();
    if (impl_->due(*job))
        return request_stop(job->id, ErrorCode::job_timeout);
    if (job->attempt == std::numeric_limits<std::uint64_t>::max() ||
        impl_->attempts >= impl_->limits.attempts)
        return rejected(ErrorCode::budget_exceeded);
    auto admitted = event("attempt.retry_admitted", job->id, job->attempt + 1);
    admitted.attempt_id = AttemptId{job->attempt + 1};
    auto outcome = event("attempt.outcome", job->id, ticket.dispatch_generation);
    outcome.external_outcome = previous_outcome;
    return impl_->commit(
        {std::move(outcome), std::move(admitted), changed(job->id, job->state, JobState::queued)},
        [&]() noexcept {
            ++job->attempt;
            ++impl_->attempts;
            job->in_flight = false;
            job->state = JobState::queued;
            job->not_before = not_before;
        });
}

ManualControlExecutor::ManualControlExecutor(std::size_t normal, std::size_t mandatory)
    : normal_capacity_(normal), mandatory_capacity_(mandatory) {
    normal_.reserve(normal);
    mandatory_.reserve(mandatory);
}
Result<void> ManualControlExecutor::post(ControlCommand command, bool mandatory) {
    auto &queue = mandatory ? mandatory_ : normal_;
    if (!mandatory && normal_closed_)
        return rejected(ErrorCode::capability_unavailable);
    if (mandatory)
        for (const auto &item : queue)
            if (item.action == command.action && item.ticket == command.ticket &&
                item.generation == command.generation)
                return Result<void>::success();
    if (queue.size() >= (mandatory ? mandatory_capacity_ : normal_capacity_))
        return rejected(ErrorCode::budget_exceeded);
    queue.push_back(command);
    return Result<void>::success();
}
Result<bool> ManualControlExecutor::step(RunController &controller) {
    auto &queue = mandatory_.empty() ? normal_ : mandatory_;
    if (queue.empty())
        return Result<bool>::success(false);
    auto command = queue.front();
    if (command.ticket.job.run() != controller.id())
        return Result<bool>::failure(error(ErrorCode::invalid_request));
    queue.erase(queue.begin());
    Result<void> result = rejected();
    switch (command.action) {
    case ControlAction::queue:
        result = controller.queue(command.ticket.job);
        break;
    case ControlAction::cancel:
        result = controller.request_stop(command.ticket.job);
        break;
    case ControlAction::complete:
        result = controller.complete(command.ticket);
        break;
    case ControlAction::wake:
        result = controller.wake(command.ticket.job, command.generation);
        break;
    case ControlAction::cleanup_released:
        result = controller.acknowledge_cleanup(command.ticket.job, {true, false, false, false});
        break;
    }
    if (!result)
        return Result<bool>::failure(result.error());
    return Result<bool>::success(true);
}
void ManualControlExecutor::close_normal_ingress() noexcept { normal_closed_ = true; }
std::size_t ManualControlExecutor::pending() const noexcept {
    return normal_.size() + mandatory_.size();
}

} // namespace flamoris::runtime
