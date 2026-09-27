#include "catch_amalgamated.hpp"
#include "flamoris/runtime/lifecycle.hpp"
#include "flamoris/runtime/observation.hpp"
#include "support/deterministic.hpp"
#include <algorithm>
#include <type_traits>

using namespace flamoris::runtime;
using namespace flamoris::runtime::testing;
using namespace std::chrono_literals;

namespace {
constexpr DispatchChecks allowed{true, true, true, true};
struct Fixture {
  ManualClock clock;
  std::unique_ptr<RunController> run;
  explicit Fixture(bool pausable = true, TimePoint duration = 100s, LifecycleLimits limits = {}) {
    auto created = RunController::create(RunId{RuntimeInstanceId{1, 2}, 1}, limits,
        Deadline::at(duration), clock, pausable);
    REQUIRE(created); run = std::move(created).value();
  }
  DispatchTicket start(JobId id) {
    REQUIRE(run->queue(id)); auto ticket = run->dispatch(id, allowed); REQUIRE(ticket); return ticket.value();
  }
  DispatchTicket start() { return start(run->root()); }
  JobSnapshot root() { return run->job(run->root()).value(); }
  void clean(JobId id) {
    REQUIRE(run->acknowledge_cleanup(id, {true, false, false, false})); REQUIRE(run->finalize(id));
  }
};
ResumePayload payload(std::uint64_t reference = 11) {
  ResumePayload result; result.state_reference = reference; result.state_version = 7; return result;
}
}

TEST_CASE("B-LIFE01 complete transition table and move-only continuation ownership") {
  static_assert(!std::is_copy_constructible_v<ContinuationState>);
  static_assert(!std::is_copy_constructible_v<PendingResume>);
  const std::vector<std::pair<JobState, JobState>> allowed_pairs{
      {JobState::created,JobState::queued},{JobState::created,JobState::cancelling},
      {JobState::queued,JobState::running},{JobState::queued,JobState::paused},{JobState::queued,JobState::cancelling},
      {JobState::running,JobState::waiting},{JobState::running,JobState::paused},{JobState::running,JobState::queued},
      {JobState::running,JobState::finalizing},{JobState::running,JobState::cancelling},
      {JobState::waiting,JobState::queued},{JobState::waiting,JobState::paused},{JobState::waiting,JobState::cancelling},
      {JobState::paused,JobState::waiting},{JobState::paused,JobState::queued},{JobState::paused,JobState::cancelling},
      {JobState::cancelling,JobState::finalizing},{JobState::finalizing,JobState::succeeded},
      {JobState::finalizing,JobState::failed},{JobState::finalizing,JobState::cancelled}};
  for (int from = 0; from <= 9; ++from) for (int to = 0; to <= 9; ++to) {
    auto pair = std::pair{static_cast<JobState>(from),static_cast<JobState>(to)};
    REQUIRE(is_allowed_transition(pair.first,pair.second) == (std::find(allowed_pairs.begin(),allowed_pairs.end(),pair) != allowed_pairs.end()));
  }
  Fixture fixture;
  const auto before = fixture.run->snapshot().watermark;
  REQUIRE_FALSE(fixture.run->dispatch(fixture.run->root(),allowed));
  REQUIRE_FALSE(fixture.run->wake(fixture.run->root(),1));
  REQUIRE_FALSE(fixture.run->finalize(fixture.run->root()));
  REQUIRE(fixture.run->snapshot().watermark == before);
}

TEST_CASE("A08 A09 suspended state moves once into same queued Job then current dispatch") {
  Fixture f; const auto first = f.start();
  auto suspended = f.run->suspend(first,payload(),{true,true}); REQUIRE(suspended);
  REQUIRE(f.root().state == JobState::waiting); REQUIRE(f.root().continuation);
  REQUIRE_FALSE(f.root().execution_in_flight);
  REQUIRE(f.run->wake(first.job,suspended.value()));
  REQUIRE_FALSE(f.root().continuation); REQUIRE(f.root().pending_resume);
  REQUIRE(f.root().state_reference == 11);
  const auto watermark = f.run->snapshot().watermark;
  REQUIRE_FALSE(f.run->wake(first.job,suspended.value()));
  REQUIRE(f.run->snapshot().watermark == watermark);
  REQUIRE_FALSE(f.run->dispatch(first.job,{true,true,true,false}));
  REQUIRE(f.root().pending_resume); REQUIRE(f.root().state_reference == 11);
  auto second = f.run->dispatch(first.job,allowed); REQUIRE(second);
  REQUIRE(second.value().job == first.job); REQUIRE(second.value().attempt == first.attempt);
  REQUIRE(second.value().dispatch_generation > first.dispatch_generation);
  REQUIRE_FALSE(f.run->ticket_is_current(first)); REQUIRE_FALSE(f.root().pending_resume);
  REQUIRE(f.root().state_reference == 11); REQUIRE_FALSE(f.run->complete(first));
}

TEST_CASE("A14 A15 completion and cancellation are controller commit ordered") {
  SECTION("success first freezes finalizing") {
    Fixture f; auto ticket = f.start(); REQUIRE(f.run->complete(ticket));
    REQUIRE(f.run->request_stop(ticket.job));
    REQUIRE(f.root().terminal_intent == JobState::succeeded); f.clean(ticket.job);
    REQUIRE(f.root().state == JobState::succeeded);
  }
  SECTION("cancel first rejects late success") {
    Fixture f; auto ticket = f.start(); REQUIRE(f.run->request_stop(ticket.job));
    REQUIRE(f.root().state == JobState::cancelling); REQUIRE_FALSE(f.run->complete(ticket));
    REQUIRE(f.run->observe_stopped(ticket,{true,false,ExternalOutcome::confirmed_success}));
    f.clean(ticket.job); REQUIRE(f.root().state == JobState::cancelled);
  }
  SECTION("uncertain cancellation fails without rollback claim") {
    Fixture f; auto ticket = f.start(); REQUIRE(f.run->request_stop(ticket.job));
    REQUIRE(f.run->observe_stopped(ticket,{true,false,ExternalOutcome::unknown}));
    f.clean(ticket.job); REQUIRE(f.root().state == JobState::failed);
    REQUIRE(f.root().error == ErrorCode::outcome_unknown);
  }
}

TEST_CASE("A17 A38 exact deadline and separate cleanup finalization authority") {
  SECTION("completion at deadline cannot beat delayed timer") {
    Fixture f(true,100ns); auto ticket=f.start(); REQUIRE(f.clock.advance(100ns));
    REQUIRE(f.run->complete(ticket)); REQUIRE(f.root().state == JobState::cancelling);
    REQUIRE(f.root().error == ErrorCode::job_timeout);
    REQUIRE(f.run->observe_stopped(ticket,{true,false,ExternalOutcome::unknown}));
    REQUIRE(f.root().error == ErrorCode::job_timeout);
  }
  SECTION("accepted success ignores workload expiry during cleanup") {
    Fixture f(true,100ns); auto ticket=f.start(); REQUIRE(f.clock.advance(99ns));
    REQUIRE(f.run->complete(ticket)); REQUIRE(f.clock.advance(100ns));
    REQUIRE(f.run->check_deadlines()); REQUIRE(f.run->cancel_run());
    REQUIRE(f.root().terminal_intent == JobState::succeeded);
    REQUIRE_FALSE(f.run->finalize(ticket.job));
    f.clean(ticket.job); REQUIRE(f.root().state == JobState::succeeded);
  }
}

TEST_CASE("A16 A37 targeted pause retains pending result and Run resume cannot clear it") {
  Fixture f; auto ticket=f.start();
  auto child=f.run->register_child(ticket.job,Deadline::at(50s),true); REQUIRE(child);
  auto state=payload(); state.wait_set.push_back(child.value()); state.wait_satisfied=false;
  auto generation=f.run->suspend(ticket,std::move(state),{true,true}); REQUIRE(generation);
  REQUIRE(f.run->pause_job(ticket.job,15)); REQUIRE(f.run->wake(ticket.job,generation.value()));
  REQUIRE(f.root().state == JobState::paused); REQUIRE(f.root().continuation);
  REQUIRE(f.root().wait_satisfied);
  REQUIRE(f.run->resume_run(20,true)); REQUIRE(f.root().state == JobState::paused);
  REQUIRE(f.run->snapshot().activity == RunActivity::waiting);
  REQUIRE(f.run->resume_job(ticket.job,15,true)); REQUIRE(f.root().pending_resume);
  REQUIRE(f.root().state == JobState::queued);
}

TEST_CASE("A37 Run barrier only removes its own causes") {
  Fixture f; REQUIRE(f.run->queue(f.run->root())); REQUIRE(f.run->pause_job(f.run->root(),7));
  REQUIRE(f.run->pause_run(8,Deadline::at(20s))); REQUIRE(f.root().pause_causes == 2);
  REQUIRE(f.run->snapshot().activity == RunActivity::paused);
  REQUIRE(f.run->resume_run(9,true)); REQUIRE(f.root().state == JobState::paused);
  REQUIRE(f.root().pause_causes == 1); REQUIRE(f.run->snapshot().activity == RunActivity::waiting);
  REQUIRE(f.run->resume_job(f.run->root(),7,true)); REQUIRE(f.root().state == JobState::queued);
}

TEST_CASE("A40 pause barrier preflight and child registration have one commit boundary") {
  SECTION("existing nonpausable child rejects entire barrier") {
    Fixture f; auto parent=f.start(); auto child=f.run->register_child(parent.job,Deadline::at(50s),false); REQUIRE(child);
    REQUIRE(f.run->queue(child.value()));
    REQUIRE_FALSE(f.run->pause_run(1,Deadline::at(20s)));
    REQUIRE(f.run->snapshot().dispatch_open); REQUIRE(f.run->snapshot().child_creation_open);
    REQUIRE_FALSE(f.root().pause_requested);
  }
  SECTION("later child cannot cross accepted gate") {
    Fixture f; auto parent=f.start(); REQUIRE(f.run->pause_run(1,Deadline::at(20s)));
    REQUIRE_FALSE(f.run->register_child(parent.job,Deadline::at(50s),true));
    REQUIRE(f.run->snapshot().pause_barrier_pending);
    REQUIRE(f.run->suspend(parent,payload(),{true,true}));
    REQUIRE(f.run->snapshot().activity == RunActivity::paused);
    REQUIRE_FALSE(f.run->snapshot().pause_barrier_pending);
  }
}

TEST_CASE("A28 no stop or containment evidence means no terminal or fake release") {
  Fixture f; auto ticket=f.start(); REQUIRE(f.run->request_stop(ticket.job));
  REQUIRE_FALSE(f.run->observe_stopped(ticket,{false,false,ExternalOutcome::not_applicable}));
  REQUIRE(f.root().state == JobState::cancelling); REQUIRE(f.root().execution_in_flight);
  REQUIRE_FALSE(f.run->finalize(ticket.job));
  REQUIRE(f.run->observe_stopped(ticket,{false,true,ExternalOutcome::not_applicable}));
  REQUIRE_FALSE(f.run->acknowledge_cleanup(ticket.job,{false,true,true,false}));
  REQUIRE(f.root().cleanup_pending);
  REQUIRE(f.run->acknowledge_cleanup(ticket.job,{false,true,true,true}));
  REQUIRE(f.run->finalize(ticket.job)); REQUIRE(f.root().state == JobState::cancelled);
}

TEST_CASE("A07 child registration and suspension atomic preparation rollback") {
  Fixture f; auto ticket=f.start(); std::vector<ChildSpec> specs{{Deadline::at(50s),true},{Deadline::at(50s),false}};
  auto before=f.run->snapshot(); f.run->fail_next_preparation();
  REQUIRE_FALSE(f.run->spawn_and_suspend(ticket,specs,payload(),{true,true}));
  REQUIRE(f.run->snapshot().watermark == before.watermark); REQUIRE(f.run->snapshot().jobs.size() == 1);
  REQUIRE(f.root().state == JobState::running);
  auto spawned=f.run->spawn_and_suspend(ticket,specs,payload(),{true,true}); REQUIRE(spawned);
  REQUIRE(f.run->snapshot().jobs.size() == 3); REQUIRE(f.root().state == JobState::waiting);
  const auto& group=f.run->events().back();
  REQUIRE(group.events[0].kind == "continuation.created");
  REQUIRE(group.events[1].to == JobState::waiting);
  REQUIRE(group.events[2].kind == "job.created");
  for (auto child:spawned.value().children) REQUIRE(f.run->job(child).value().parent == ticket.job);
}

TEST_CASE("A19 descendants settle before parent terminal") {
  Fixture f; auto parent=f.start(); auto child=f.run->register_child(parent.job,Deadline::at(50s),true); REQUIRE(child);
  auto child_ticket=f.start(child.value()); REQUIRE(f.run->complete(parent));
  REQUIRE(f.run->acknowledge_cleanup(parent.job,{true,false,false,false}));
  REQUIRE_FALSE(f.run->finalize(parent.job)); REQUIRE(f.run->complete(child_ticket));
  f.clean(child.value()); REQUIRE(f.run->finalize(parent.job));
  const auto& terminal=f.run->events().back().events;
  REQUIRE(terminal[terminal.size()-2].kind == "run.terminal");
  REQUIRE(terminal.back().run_to == RunActivity::succeeded);
}

TEST_CASE("B-EVENT01 failed preparation exposes no state or partial group") {
  Fixture f; auto before=f.run->snapshot(); f.run->fail_next_preparation();
  REQUIRE_FALSE(f.run->queue(f.run->root())); REQUIRE(f.root().state == JobState::created);
  REQUIRE(f.run->snapshot().watermark == before.watermark);
  REQUIRE(f.run->observation_store().watermark() == before.watermark);
  REQUIRE(f.run->queue(f.run->root()));
  for (const auto& group:f.run->events()) for (std::size_t i=0;i<group.events.size();++i)
    REQUIRE(group.events[i].sequence == group.first_sequence+i);
}

TEST_CASE("B-RETRY01 retry preserves Job deadline while fencing previous attempt") {
  Fixture f; auto old=f.start(); REQUIRE_FALSE(f.run->retry(old,1s,true,false,true));
  REQUIRE(f.run->retry(old,1s,true,true,true)); REQUIRE(f.root().state == JobState::queued);
  REQUIRE_FALSE(f.run->dispatch(old.job,allowed)); REQUIRE(f.clock.advance(1s));
  auto next=f.run->dispatch(old.job,allowed); REQUIRE(next); REQUIRE(next.value().attempt == old.attempt+1);
  REQUIRE(next.value().job == old.job); REQUIRE(f.root().deadline.time() == 100s);
  REQUIRE_FALSE(f.run->complete(old)); REQUIRE(f.run->complete(next.value()));
  REQUIRE_FALSE(f.run->retry(next.value(),2s,true,true,true));
}

TEST_CASE("A30 normal ingress exhaustion cannot occupy completion and stop slots") {
  Fixture f; auto ticket=f.start(); ManualControlExecutor executor(1,2);
  REQUIRE(executor.post({ControlAction::wake,ticket,999}));
  REQUIRE_FALSE(executor.post({ControlAction::wake,ticket,998}));
  REQUIRE(executor.post({ControlAction::cancel,ticket,0},true));
  REQUIRE(executor.post({ControlAction::cancel,ticket,0},true)); REQUIRE(executor.pending() == 2);
  executor.close_normal_ingress(); REQUIRE_FALSE(executor.post({ControlAction::queue,ticket,0}));
  REQUIRE(executor.step(*f.run)); REQUIRE(f.root().state == JobState::cancelling);
}

TEST_CASE("A08 resume preserves the one admitted attempt budget") {
  LifecycleLimits limits; limits.attempts=1;
  Fixture f(true,100s,limits); auto first=f.start();
  auto generation=f.run->suspend(first,payload(),{true,true}); REQUIRE(generation);
  REQUIRE(f.run->wake(first.job,generation.value()));
  auto second=f.run->dispatch(first.job,allowed); REQUIRE(second);
  REQUIRE(second.value().attempt==first.attempt);
  REQUIRE_FALSE(f.run->retry(second.value(),1s,true,true,true));
}

TEST_CASE("B-LIFE01 optional zero budgets permit cancellation and invalid admission stays typed") {
  LifecycleLimits limits; limits.suspensions=0; limits.commands=0;
  Fixture f(false,100s,limits); auto ticket=f.start();
  REQUIRE_FALSE(f.run->pause_job(ticket.job,1)); REQUIRE(f.run->cancel_run());
  REQUIRE(f.run->observe_stopped(ticket,{true,false,ExternalOutcome::not_applicable}));
  f.clean(ticket.job); REQUIRE(f.root().state==JobState::cancelled);
  ManualClock clock;
  REQUIRE_FALSE(RunController::create(RunId{},limits,Deadline::at(1s),clock));
  limits.event_slots=1'000'000;
  REQUIRE_FALSE(RunController::create(RunId{RuntimeInstanceId{1,2},1},limits,Deadline::at(1s),clock));
}

TEST_CASE("A28 cleanup deadline reports retained ownership once without inventing stopped state") {
  LifecycleLimits limits;limits.cleanup_allowance=1s;
  Fixture f(true,100s,limits);auto ticket=f.start();REQUIRE(f.run->request_stop(ticket.job));
  REQUIRE(f.clock.advance(1s));REQUIRE(f.run->check_deadlines());
  const auto watermark=f.run->snapshot().watermark;
  REQUIRE(f.run->check_deadlines());REQUIRE(f.run->snapshot().watermark==watermark);
  REQUIRE(f.root().state==JobState::cancelling);REQUIRE(f.root().execution_in_flight);REQUIRE(f.root().cleanup_pending);
  bool timeout=false;for(const auto& group:f.run->events())for(const auto& e:group.events)
    timeout|=e.kind=="cleanup.pending" && e.error==ErrorCode::cleanup_timeout;
  REQUIRE(timeout);REQUIRE_FALSE(f.run->finalize(ticket.job));
}
