#pragma once

#include "flamoris/runtime/clock.hpp"
#include "flamoris/runtime/lifecycle_events.hpp"
#include "flamoris/runtime/result.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace flamoris::runtime {
class EventStore;
enum class EventStorageClass;

struct LifecycleLimits {
    std::size_t jobs{32};
    std::size_t attempts{128};
    std::size_t suspensions{128};
    std::size_t commands{128};
    std::size_t event_slots{8192};
    std::size_t pause_causes_per_job{16};
    TimePoint cleanup_allowance{std::chrono::seconds(5)};
};

// Move-only references: actual native allocations are owned and charged by the
// worker/ResourceManager, not freed by moving or destroying this descriptor.
struct ResumePayload {
    std::uint64_t state_reference{};
    std::uint64_t state_version{};
    std::vector<JobId> wait_set;
    bool wait_satisfied{true};
    ResumePayload() = default;
    ResumePayload(ResumePayload &&) noexcept = default;
    ResumePayload &operator=(ResumePayload &&) noexcept = default;
    ResumePayload(const ResumePayload &) = delete;
    ResumePayload &operator=(const ResumePayload &) = delete;
};

struct ContinuationState {
    JobId owner;
    std::uint64_t suspension_generation{};
    ResumePayload payload;
    ContinuationState(JobId id, std::uint64_t generation, ResumePayload value)
        : owner(id), suspension_generation(generation), payload(std::move(value)) {}
    ContinuationState(ContinuationState &&) noexcept = default;
    ContinuationState &operator=(ContinuationState &&) noexcept = default;
    ContinuationState(const ContinuationState &) = delete;
    ContinuationState &operator=(const ContinuationState &) = delete;
};

struct PendingResume {
    std::uint64_t suspension_generation{};
    ResumePayload payload;
    PendingResume(std::uint64_t generation, ResumePayload value)
        : suspension_generation(generation), payload(std::move(value)) {}
    PendingResume(PendingResume &&) noexcept = default;
    PendingResume &operator=(PendingResume &&) noexcept = default;
    PendingResume(const PendingResume &) = delete;
    PendingResume &operator=(const PendingResume &) = delete;
};

struct DispatchTicket {
    JobId job;
    std::uint64_t attempt{};
    std::uint64_t dispatch_generation{};
    auto operator<=>(const DispatchTicket &) const = default;
};

// Supplied only by the composition root after checking current policy, plan
// pins and the full resource grant in the same serialized control turn.
struct DispatchChecks {
    bool authorized{};
    bool pins_current{};
    bool state_valid{};
    bool resources_granted{};
};
struct SafePointEvidence {
    bool quiescent{};
    bool preserved{};
};
struct StopEvidence {
    bool quiescent{};
    bool safely_contained{};
    ExternalOutcome external_outcome{ExternalOutcome::not_applicable};
};
struct CleanupEvidence {
    bool released{};
    bool transferred{};
    bool safely_contained{};
    bool closure_reserved{};
};
struct ChildSpec {
    Deadline deadline;
    bool pause_supported{false};
};
struct SpawnResult {
    std::vector<JobId> children;
    std::uint64_t suspension_generation{};
};

struct JobSnapshot {
    JobId id;
    std::optional<JobId> parent;
    JobState state{JobState::created};
    std::optional<JobState> terminal_intent;
    std::optional<ErrorCode> error;
    Deadline deadline;
    std::uint64_t attempt{};
    std::uint64_t dispatch_generation{};
    std::uint64_t suspension_generation{};
    std::uint64_t state_reference{};
    std::uint64_t state_version{};
    bool continuation{};
    bool pending_resume{};
    bool execution_in_flight{};
    bool cleanup_pending{};
    bool pause_requested{};
    bool pause_supported{};
    bool wait_satisfied{};
    std::size_t pause_causes{};
};
struct RunSnapshot {
    RunId id;
    JobId root;
    RunActivity activity{RunActivity::created};
    std::uint64_t watermark{};
    bool dispatch_open{true};
    bool child_creation_open{true};
    std::uint64_t pause_barrier_generation{};
    bool pause_barrier_pending{};
    std::vector<JobSnapshot> jobs;
};

// Single control-executor owner. No method invokes workers or callbacks. All
// fallible group allocation is prepared before noexcept ownership/state moves.
class RunController {
  public:
    static Result<std::unique_ptr<RunController>>
    create(RunId, LifecycleLimits, Deadline, MonotonicClock &, bool root_pause_supported = true);
    ~RunController();
    RunController(const RunController &) = delete;
    RunController &operator=(const RunController &) = delete;
    [[nodiscard]] JobId root() const noexcept;
    [[nodiscard]] RunId id() const noexcept;
    [[nodiscard]] RunSnapshot snapshot() const;
    [[nodiscard]] Result<JobSnapshot> job(JobId) const;
    [[nodiscard]] const std::vector<EventGroup> &events() const noexcept;
    [[nodiscard]] EventStore &observation_store() noexcept;
    Result<void> commit_observation(EventGroup, EventStorageClass, std::uint64_t reservation = 0);
    [[nodiscard]] bool ticket_is_current(const DispatchTicket &) const noexcept;

    Result<JobId> register_child(JobId parent, Deadline, bool pause_supported);
    Result<void> queue(JobId);
    Result<DispatchTicket> dispatch(JobId, DispatchChecks);
    Result<std::uint64_t> suspend(DispatchTicket, ResumePayload, SafePointEvidence);
    Result<SpawnResult> spawn_and_suspend(DispatchTicket, std::span<const ChildSpec>, ResumePayload,
                                          SafePointEvidence);
    Result<void> wake(JobId, std::uint64_t suspension_generation);
    Result<void> pause_job(JobId, std::uint64_t command_id);
    Result<void> resume_job(JobId, std::uint64_t pause_command_id, bool authorized);
    Result<void> pause_run(std::uint64_t command_id, Deadline request_deadline);
    Result<void> resume_run(std::uint64_t command_id, bool authorized);
    Result<void> expire_pause_barrier();
    Result<void> complete(DispatchTicket, ExternalOutcome = ExternalOutcome::not_applicable);
    Result<void> request_stop(JobId, ErrorCode cause = ErrorCode::job_cancelled);
    Result<void> cancel_run(ErrorCode cause = ErrorCode::run_cancelled);
    Result<void> observe_stopped(DispatchTicket, StopEvidence);
    Result<void> acknowledge_cleanup(JobId, CleanupEvidence);
    Result<void> finalize(JobId);
    Result<void> check_deadlines();
    Result<void> retry(DispatchTicket, TimePoint not_before, bool explicitly_authorized,
                       bool previous_stopped, bool reconciled,
                       ExternalOutcome previous_outcome = ExternalOutcome::not_applicable);

    // Deterministic preparation failure seam, consumed before any mutation.
    void fail_next_preparation() noexcept;
    void fail_preparation_after(std::size_t successful_preparations) noexcept;

  private:
    struct Impl;
    explicit RunController(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

enum class ControlAction { queue, cancel, complete, wake, cleanup_released };
struct ControlCommand {
    ControlAction action{ControlAction::queue};
    DispatchTicket ticket;
    std::uint64_t generation{};
};

// A deterministic bounded transport. Mandatory completion/stop slots are
// separate from normal admission; duplicate observations coalesce by ticket.
class ManualControlExecutor {
  public:
    explicit ManualControlExecutor(std::size_t normal_capacity, std::size_t mandatory_capacity);
    Result<void> post(ControlCommand, bool mandatory = false);
    Result<bool> step(RunController &);
    void close_normal_ingress() noexcept;
    [[nodiscard]] std::size_t pending() const noexcept;

  private:
    std::vector<ControlCommand> normal_;
    std::vector<ControlCommand> mandatory_;
    std::size_t normal_capacity_;
    std::size_t mandatory_capacity_;
    bool normal_closed_{};
};

} // namespace flamoris::runtime
