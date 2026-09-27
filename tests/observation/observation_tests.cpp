#include "catch_amalgamated.hpp"
#include "flamoris/runtime/observation.hpp"
#include "flamoris/runtime/authorization.hpp"
#include "flamoris/runtime/lifecycle.hpp"
#include "support/deterministic.hpp"
#include <type_traits>

using namespace flamoris::runtime;
namespace {
const RunId run{RuntimeInstanceId{7, 9}, 1};
const JobId job{run, 1};
EventGroup group(std::uint64_t first, std::size_t count = 1,
                 std::string kind = "job.state_changed") {
  EventGroup result{first, first, {}};
  for (std::size_t i = 0; i < count; ++i) {
    LifecycleEvent event;
    event.sequence = first + i; event.kind = kind; event.job = job;
    if (kind == "job.state_changed") event.to = JobState::created;
    result.events.push_back(std::move(event));
  }
  return result;
}
void append(EventStore& store, EventGroup value, EventStorageClass storage = EventStorageClass::control,
            std::uint64_t reservation = 0) {
  auto prepared = store.prepare(std::move(value), storage, reservation);
  REQUIRE(prepared);
  REQUIRE(store.publish(std::move(prepared).value()));
}
ObservationLimits small_limits() {
  ObservationLimits limits;
  limits.max_groups = 16; limits.control_events = 8; limits.emergency_events = 2;
  limits.reconciliation_events = 4; limits.telemetry_events = 2;
  limits.max_bytes = 16 * 8192; limits.max_subscribers = 1; limits.max_reconciliations = 2;
  return limits;
}
}

TEST_CASE("A30 EventStore preserves prepaid stop and closure storage under telemetry and normal exhaustion") {
  EventStore store(small_limits());
  auto reserve = store.reserve_reconciliation(1);
  REQUIRE(reserve);
  append(store, group(1, 2, "token.generated"), EventStorageClass::telemetry);
  REQUIRE_FALSE(store.prepare(group(3, 1, "token.generated"), EventStorageClass::telemetry));
  append(store, group(3, 8));
  REQUIRE_FALSE(store.prepare(group(11)));
  append(store, group(11, 2, "interrupt.requested"), EventStorageClass::emergency);
  append(store, group(13, 1, "reconciliation.observed"), EventStorageClass::reconciliation, reserve.value());
  REQUIRE_FALSE(store.close());
  append(store, group(14, 1, "reconciliation.closed"), EventStorageClass::reconciliation, reserve.value());
  REQUIRE(store.close());
  const auto page = store.read(0);
  REQUIRE(page);
  REQUIRE(page.value().telemetry_dropped == 1);
  REQUIRE(page.value().watermark == 14);
  REQUIRE(store.retained_bytes() <= small_limits().max_bytes);
  REQUIRE_FALSE(store.prepare(group(15)));
}

TEST_CASE("B-EVENT01 preparation has no publication and duplicate publish fails closed") {
  EventStore store(small_limits());
  auto prepared = store.prepare(group(1, 2));
  REQUIRE(prepared);
  REQUIRE(store.watermark() == 0);
  REQUIRE(store.groups().empty());
  REQUIRE(store.publish(std::move(prepared).value()));
  REQUIRE(store.watermark() == 2);
  REQUIRE(store.groups().size() == 1);
  auto invalid = group(3); invalid.events.front().kind = "https://private/credential";
  REQUIRE_FALSE(store.prepare(std::move(invalid)));
  REQUIRE(store.watermark() == 2);
  auto first = store.prepare(group(3));
  auto raced = store.prepare(group(3));
  REQUIRE(first); REQUIRE(raced);
  REQUIRE(store.publish(std::move(first).value()));
  REQUIRE_FALSE(store.publish(std::move(raced).value()));
  REQUIRE_FALSE(store.healthy());
  REQUIRE(store.watermark() == 3);
}

TEST_CASE("A31 complete group pagination cursor gaps and independent bounded subscribers") {
  EventStore store(small_limits());
  auto subscriber = store.subscribe(0, 2);
  REQUIRE(subscriber);
  REQUIRE_FALSE(store.subscribe(0, 2));
  append(store, group(1, 2)); append(store, group(3, 2)); append(store, group(5, 2));
  auto too_small = store.read(0, 1);
  REQUIRE(too_small); REQUIRE(too_small.value().groups.empty());
  REQUIRE(too_small.value().incomplete_group->expected_events == 2);
  auto mid_group = store.read(1);
  REQUIRE(mid_group); REQUIRE(mid_group.value().gap); REQUIRE(mid_group.value().incomplete_group);
  REQUIRE(mid_group.value().groups.front().first_sequence == 3);
  auto slow = store.poll(subscriber.value());
  REQUIRE(slow); REQUIRE(slow.value().gap->subscriber_overflow);
  REQUIRE(slow.value().groups.size() == 1);
  REQUIRE(slow.value().groups.front().first_sequence == 5);
  REQUIRE(store.watermark() == 6);
  store.retire_through(3); // A midpoint never retires half a committed group.
  REQUIRE(store.earliest_sequence() == 3);
  auto gap = store.read(0);
  REQUIRE(gap.value().gap->earliest_available == 3);
  REQUIRE(gap.value().watermark == 6);
  REQUIRE_FALSE(store.read(2).value().gap);
  REQUIRE(store.read(6).value().groups.empty());
  REQUIRE(store.expire());
  REQUIRE(store.read(6).value().gap->expired);
  REQUIRE_FALSE(store.prepare(group(7)));
  store.unsubscribe(subscriber.value());
  REQUIRE(store.subscriber_count() == 0);
}

TEST_CASE("B-EVENT01 retention cannot erase incarnation transition or publication ownership fences") {
  EventStore first(small_limits()), second(small_limits());
  append(first, group(1)); first.retire_through(1);
  auto wrong_run = group(2); wrong_run.events.front().job = JobId{RunId{RuntimeInstanceId{8, 9}, 1}, 1};
  REQUIRE_FALSE(first.prepare(std::move(wrong_run)));
  auto reused_transition = group(2); reused_transition.transition = 1;
  REQUIRE_FALSE(first.prepare(std::move(reused_transition)));
  auto other_owner = first.prepare(group(2)); REQUIRE(other_owner);
  REQUIRE_FALSE(second.publish(std::move(other_owner).value()));
  REQUIRE(second.watermark() == 0);
  REQUIRE_FALSE(second.healthy());
  REQUIRE(first.watermark() == 1);
}

TEST_CASE("A29 reconciliation capacity is reserved before transfer and remains bounded after terminal") {
  auto limits = small_limits(); limits.reconciliation_events = 2;
  EventStore store(limits);
  auto ticket = store.reserve_reconciliation(1);
  REQUIRE(ticket);
  REQUIRE_FALSE(store.reserve_reconciliation(0));
  append(store, group(1, 1, "run.terminal"));
  auto observed = group(2, 1, "reconciliation.observed");
  observed.events.front().operation = 41; observed.events.front().generation = 8;
  observed.events.front().ledger_revision = 12;
  append(store, std::move(observed), EventStorageClass::reconciliation, ticket.value());
  REQUIRE_FALSE(store.prepare(group(3, 1, "reconciliation.observed"), EventStorageClass::reconciliation, ticket.value()));
  append(store, group(3, 1, "reconciliation.closed"), EventStorageClass::reconciliation, ticket.value());
  REQUIRE_FALSE(store.reconciliation_reserved(ticket.value()));
  REQUIRE(store.expire());
  REQUIRE_FALSE(store.prepare(group(4, 1, "reconciliation.observed"), EventStorageClass::reconciliation, ticket.value()));
  REQUIRE(store.watermark() == 3);
}

TEST_CASE("A32 replay is a separate projection and excludes incomplete unsupported redacted groups") {
  EventStore live(small_limits());
  append(live, group(1));
  append(live, group(2, 1, "attempt.dispatch_committed"));
  append(live, group(3, 1, "resource.allocation_unknown"));
  append(live, group(4, 1, "interrupt.requested"));
  const auto watermark = live.watermark();
  ReplayProjection replay;
  for (const auto& retained : live.groups()) REQUIRE(replay.apply({"flamoris.event/1", retained, retained.events.size(), false}));
  REQUIRE(replay.snapshot().playback);
  REQUIRE(replay.snapshot().complete);
  REQUIRE(replay.snapshot().watermark == 4);
  REQUIRE(replay.apply({"flamoris.event/1", group(5), 2, false}));
  REQUIRE(replay.apply({"future/2", group(6), 1, false}));
  REQUIRE(replay.apply({"flamoris.event/1", group(7), 1, true}));
  REQUIRE_FALSE(replay.snapshot().complete);
  REQUIRE(replay.snapshot().watermark == 4);
  REQUIRE(replay.snapshot().rejected_groups == 3);
  REQUIRE(replay.apply({"flamoris.event/1", group(8, 1, "reconciliation.closed"), 1, false}));
  REQUIRE(replay.snapshot().watermark == 8);
  REQUIRE(live.watermark() == watermark);
  REQUIRE(live.groups().size() == 4);
  // No execution interface is accepted by this production projection.
  static_assert(!std::is_constructible_v<ReplayProjection, RunController&>);
}

TEST_CASE("A35 observation surfaces independently revalidate current scope and expiry") {
  EventStore store(small_limits()); append(store, group(1));
  AuthorizedObservation observation(store, "owner");
  AuthorizationContext context; context.subject = "owner"; context.expires_at_ms = 100;
  context.access = {AccessSurface::events, AccessSurface::export_data, AccessSurface::replay};
  PolicySnapshot policy; policy.access = context.access;
  REQUIRE(observation.read(context, policy, 1, 0));
  REQUIRE(observation.export_trace(context, policy, 1));
  REQUIRE(observation.replay(context, policy, 1));
  policy.access.erase(AccessSurface::replay);
  REQUIRE_FALSE(observation.replay(context, policy, 2));
  REQUIRE(observation.read(context, policy, 2, 0));
  policy.access.erase(AccessSurface::export_data);
  REQUIRE_FALSE(observation.export_trace(context, policy, 2));
  REQUIRE_FALSE(observation.read(context, policy, 100, 0));
  context.revoked = true;
  REQUIRE_FALSE(observation.read(context, policy, 3, 0));
  REQUIRE(store.watermark() == 1);
}
