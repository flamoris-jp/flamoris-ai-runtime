#include "catch_amalgamated.hpp"
#include "flamoris/runtime/scheduler.hpp"
#include "support/deterministic.hpp"
using namespace flamoris::runtime;
using namespace flamoris::runtime::testing;
using namespace std::chrono_literals;

TEST_CASE("A34 Scheduler ages eligible Jobs and protects older conflicting capacity") {
  ManualClock clock;
  auto created=RunController::create(RunId{RuntimeInstanceId{9,1},1},{},Deadline::at(20s),clock);REQUIRE(created);
  auto run=std::move(created).value();REQUIRE(run->queue(run->root()));
  auto young=run->register_child(run->root(),Deadline::at(4s),true);REQUIRE(young);REQUIRE(run->queue(young.value()));
  ResourceVector requirements;requirements[ResourceKind::execution]=1;
  Scheduler scheduler;REQUIRE(scheduler.ready({run->root(),0,{1},requirements,{}},*run,clock.now()));
  REQUIRE(clock.advance(1s));REQUIRE(scheduler.ready({young.value(),3,{1},requirements,{}},*run,clock.now()));
  RunController* controllers[]{run.get()};
  auto first=scheduler.select(controllers,clock.now());REQUIRE(first);REQUIRE(first.value()->job==young.value());
  REQUIRE(first.value()->acquisition_deadline.time()==4s);
  REQUIRE(clock.advance(2s));auto protected_job=scheduler.select(controllers,clock.now());REQUIRE(protected_job);
  REQUIRE(protected_job.value()->job==run->root());REQUIRE(protected_job.value()->protected_from_bypass);
  REQUIRE(protected_job.value()->acquisition_deadline.time()==8s);
  // Repeated failed capacity probes do not change readiness age/sequence.
  REQUIRE(scheduler.select(controllers,clock.now()).value()->readiness_sequence==protected_job.value()->readiness_sequence);
}

TEST_CASE("A10 A37 Scheduler excludes blocked paused expired Jobs and never schedules Continuations") {
  ManualClock clock;auto created=RunController::create(RunId{RuntimeInstanceId{9,2},1},{},Deadline::at(2s),clock);REQUIRE(created);
  auto run=std::move(created).value();Scheduler scheduler;
  REQUIRE_FALSE(scheduler.ready({run->root(),0,{1},{},{}},*run,clock.now()));
  REQUIRE(run->queue(run->root()));REQUIRE(scheduler.ready({run->root(),0,{1},{},{}},*run,clock.now()));
  REQUIRE(run->pause_job(run->root(),1));RunController* controllers[]{run.get()};
  REQUIRE_FALSE(scheduler.select(controllers,clock.now()).value());
  REQUIRE(run->resume_job(run->root(),1,true));REQUIRE(scheduler.select(controllers,clock.now()).value());
  REQUIRE(clock.advance(2s));REQUIRE_FALSE(scheduler.select(controllers,clock.now()).value());
}

TEST_CASE("A34 not-before eligibility does not accumulate backoff age") {
  ManualClock clock;auto created=RunController::create(RunId{RuntimeInstanceId{9,3},1},{},Deadline::at(20s),clock);REQUIRE(created);
  auto run=std::move(created).value();REQUIRE(run->queue(run->root()));Scheduler scheduler;
  REQUIRE(scheduler.ready({run->root(),0,{1},{},10s},*run,clock.now()));RunController* controllers[]{run.get()};
  REQUIRE_FALSE(scheduler.select(controllers,clock.now()).value());REQUIRE(clock.advance(10s));
  auto chosen=scheduler.select(controllers,clock.now());REQUIRE(chosen);REQUIRE(chosen.value()->effective_priority==0);
  REQUIRE_FALSE(chosen.value()->protected_from_bypass);
}
