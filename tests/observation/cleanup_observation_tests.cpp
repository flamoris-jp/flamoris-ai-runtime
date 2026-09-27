#include "catch_amalgamated.hpp"
#include "flamoris/runtime/cleanup_observation.hpp"
#include "flamoris/runtime/observation.hpp"
#include "support/deterministic.hpp"
#include "support/manual_resource_ports.hpp"

using namespace flamoris::runtime;
using namespace std::chrono_literals;
namespace {
struct Lookup final : RunObservationLookupPort {
  RunController* current{};
  RunController* find_observation_run(RunId id) noexcept override {
    return current && current->id() == id ? current : nullptr;
  }
};
ResourceVector footprint(std::uint64_t device, std::uint64_t execution = 0) {
  ResourceVector value; value[ResourceKind::device] = device; value[ResourceKind::execution] = execution;
  return value;
}
}
TEST_CASE("A29 real cleanup ledger appends after terminal and releases after expired observation") {
  testing::ManualClock clock;
  const RuntimeInstanceId instance{31, 17}; const RunId run{instance, 1};
  auto created = RunController::create(run, {}, Deadline::at(1000ns), clock);
  REQUIRE(created);
  auto controller = std::move(created).value();
  Lookup lookup; lookup.current = controller.get();
  ControllerCleanupObservationPort observation(lookup, clock, 1);
  test::ManualHost host;
  ResourceManager manager(instance, host, observation);
  const LogicalResourceId device{1};
  REQUIRE(manager.observe_envelope({device, instance, HostEpoch{1}, 1, 1000ns, footprint(10, 1),
      HostArbitration::enforced_generation, true, true}, 0ns));
  REQUIRE(controller->queue(controller->root()));
  ResourceRequest request{OperationId{1}, {controller->root(), AttemptId{1}, DispatchGeneration{1}},
      device, HostEpoch{1}, NativeWorkerGeneration{1}, footprint(7, 1), footprint(10, 1), 900ns, {}};
  auto reservation = manager.reserve(request, 0ns); REQUIRE(reservation);
  REQUIRE(manager.acquired({reservation.value(), instance, OperationId{2}, HostOutcome::acknowledged}));
  auto lease = manager.commit_dispatch(reservation.value(), {true, true, true, true}, 0ns); REQUIRE(lease);
  const AllocationIdentity allocation{instance, AllocationId{1}, HostEpoch{1}, NativeWorkerGeneration{1}};
  REQUIRE(manager.materialized(reservation.value(), allocation, footprint(7)));
  auto dispatched = controller->dispatch(controller->root(), {true, true, true, true}); REQUIRE(dispatched);
  REQUIRE(controller->complete(dispatched.value()));
  REQUIRE(manager.drop_reference(allocation, controller->root()));
  REQUIRE(manager.request_release(allocation, OperationId{3}));
  auto cleanup = manager.transfer_to_cleanup({allocation, lease.value(), OperationId{3},
      ContainmentProof::isolated_by_authority, {}, 100ns}, 1ns);
  REQUIRE(cleanup);
  REQUIRE(controller->acknowledge_cleanup(controller->root(), {false, true, true, true}));
  REQUIRE(controller->finalize(controller->root()));
  REQUIRE(controller->snapshot().activity == RunActivity::succeeded);
  const auto terminal = controller->snapshot().watermark;
  SECTION("matching evidence appends once to same terminal Run") {
    REQUIRE(clock.advance(2ns));
    REQUIRE(manager.release_confirmed({allocation, OperationId{3}, true}, clock.now()));
    REQUIRE(manager.quiesced({lease.value(), ContainmentProof::isolated_by_authority, true}, clock.now()));
    REQUIRE(observation.healthy());
    REQUIRE(controller->snapshot().watermark == terminal + 2);
    REQUIRE(controller->events()[controller->events().size() - 2].events.front().kind == "reconciliation.observed");
    REQUIRE(controller->events().back().events.front().kind == "reconciliation.closed");
    REQUIRE(controller->events().back().events.front().operation == cleanup.value().value());
    REQUIRE(controller->snapshot().activity == RunActivity::succeeded);
    REQUIRE_FALSE(manager.release_confirmed({allocation, OperationId{3}, true}, clock.now()).value());
    REQUIRE(controller->snapshot().watermark == terminal + 2);
    REQUIRE(manager.snapshot(device).resident.empty());
  }
  SECTION("expired Run is never reopened by authoritative ledger cleanup") {
    REQUIRE(clock.advance(101ns));
    manager.close_observations(clock.now());
    REQUIRE(observation.healthy());
    REQUIRE(controller->snapshot().watermark == terminal + 1);
    REQUIRE(controller->events().back().events.front().external_outcome == ExternalOutcome::unknown);
    REQUIRE(controller->observation_store().expire());
    lookup.current = nullptr;
    REQUIRE(manager.quiesced({lease.value(), ContainmentProof::isolated_by_authority, true}, clock.now()));
    REQUIRE(manager.release_confirmed({allocation, OperationId{3}, true}, clock.now()));
    REQUIRE(manager.snapshot(device).resident.empty());
    REQUIRE(controller->snapshot().watermark == terminal + 1);
    REQUIRE(controller->snapshot().activity == RunActivity::succeeded);
    REQUIRE(controller->observation_store().read(0).value().gap->expired);
  }
}

TEST_CASE("B-EVENT01 cleanup observation commit failure flags fail-stop without false publication") {
  testing::ManualClock clock;
  const RunId run{RuntimeInstanceId{31, 18}, 1};
  auto created = RunController::create(run, {}, Deadline::at(1000ns), clock); REQUIRE(created);
  auto controller = std::move(created).value(); Lookup lookup; lookup.current = controller.get();
  ControllerCleanupObservationPort observation(lookup, clock, 1);
  auto ticket = observation.reserve(run, CleanupId{1}, 100ns); REQUIRE(ticket);
  REQUIRE_FALSE(observation.reserve(run, CleanupId{2}, 100ns));
  const auto before = controller->snapshot().watermark;
  controller->fail_next_preparation();
  observation.observed(ticket.value(), CleanupId{1}, LedgerRevision{4});
  REQUIRE_FALSE(observation.healthy());
  REQUIRE(controller->snapshot().watermark == before);
}
