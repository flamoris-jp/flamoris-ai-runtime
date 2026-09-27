#include "../support/manual_resource_ports.hpp"
#include "flamoris/runtime/resources.hpp"
#include <catch_amalgamated.hpp>
#include <limits>

using namespace flamoris::runtime;
using namespace flamoris::runtime::test;
using namespace std::chrono_literals;
namespace {
ResourceVector resources(std::uint64_t device, std::uint64_t slots = 0, std::uint64_t ram = 0,
                         std::uint64_t transfer = 0) {
    ResourceVector v;
    v[ResourceKind::device] = device;
    v[ResourceKind::execution] = slots;
    v[ResourceKind::ram] = ram;
    v[ResourceKind::transfer] = transfer;
    return v;
}
struct Fixture {
    RuntimeInstanceId instance{5, 6};
    LogicalResourceId device{1};
    ManualHost host;
    ManualCleanupObservation observation;
    ResourceManager manager{instance, host, observation};
    HostEnvelope envelope{device,
                          instance,
                          HostEpoch{1},
                          1,
                          1000ns,
                          resources(10, 2, 20, 5),
                          HostArbitration::enforced_generation,
                          true,
                          true};
    Fixture() { REQUIRE(manager.observe_envelope(envelope, 0ns)); }
    ResourceRequest request(std::uint64_t operation, std::uint64_t run, ResourceVector incremental,
                            ResourceVector quota = resources(100, 10, 100, 10)) {
        return {OperationId{operation},
                {JobId{RunId{instance, run}, 1}, AttemptId{1}, DispatchGeneration{1}},
                device,
                HostEpoch{1},
                NativeWorkerGeneration{1},
                incremental,
                quota,
                900ns,
                {}};
    }
    std::pair<ResourceTicket, ExecutionLease> start(ResourceRequest req) {
        auto t = manager.reserve(req, 0ns);
        REQUIRE(t);
        REQUIRE(manager.acquired({t.value(), instance, OperationId{100 + req.operation.value()},
                                  HostOutcome::acknowledged}));
        auto l = manager.commit_dispatch(t.value(), {true, true, true, true}, 0ns);
        REQUIRE(l);
        return {t.value(), l.value()};
    }
    AllocationIdentity id(std::uint64_t value, std::uint64_t epoch = 1, std::uint64_t worker = 1) {
        return {instance, AllocationId{value}, HostEpoch{epoch}, NativeWorkerGeneration{worker}};
    }
};
} // namespace
TEST_CASE("C03 complete vectors, host evidence and current dispatch guards fail closed",
          "[resources][C03]") {
    Fixture f;
    auto over = f.request(1, 1, resources(11, 1));
    REQUIRE_FALSE(f.manager.reserve(over, 0ns));
    REQUIRE(f.host.acquisitions.empty());
    auto req = f.request(2, 1, resources(7, 1));
    auto ticket = f.manager.reserve(req, 0ns);
    REQUIRE(ticket);
    REQUIRE(f.manager.snapshot(f.device).reserved == resources(7, 1));
    REQUIRE_FALSE(f.manager.commit_dispatch(ticket.value(), {true, true, true, true}, 0ns));
    REQUIRE(f.manager.acquired(
        {ticket.value(), f.instance, OperationId{88}, HostOutcome::acknowledged}));
    REQUIRE_FALSE(f.manager.commit_dispatch(ticket.value(), {true, false, true, true}, 0ns));
    auto lease = f.manager.commit_dispatch(ticket.value(), {true, true, true, true}, 0ns);
    REQUIRE(lease);
    REQUIRE_FALSE(f.manager.ready_for_use(lease.value(), 0ns));
    REQUIRE(f.manager.materialized(ticket.value(), f.id(1), resources(7)));
    REQUIRE(f.manager.ready_for_use(lease.value(), 0ns));
    REQUIRE_FALSE(f.manager.commit_dispatch(ticket.value(), {true, true, true, true}, 0ns));
    REQUIRE_FALSE(f.manager.reserve(f.request(3, 2, resources(4, 1)), 0ns));
    REQUIRE(f.manager.snapshot(f.device).resident == resources(7));
    f.manager.fence(f.device);
    REQUIRE_FALSE(f.manager.ready_for_use(lease.value(), 0ns));
    auto advisory = f.envelope;
    advisory.authority_revision = 2;
    advisory.arbitration = HostArbitration::advisory;
    REQUIRE_FALSE(f.manager.observe_envelope(advisory, 0ns));
    UnavailableHostAuthority unavailable;
    ResourceManager nohost{f.instance, unavailable, f.observation};
    REQUIRE(nohost.observe_envelope(f.envelope, 0ns));
    REQUIRE_FALSE(nohost.reserve(f.request(4, 2, resources(1)), 0ns));
}
TEST_CASE("A10 retained memory survives lease release and detects dependency deadlock",
          "[resources][A10]") {
    Fixture f;
    auto [ticket, lease] = f.start(f.request(1, 1, resources(7, 1)));
    REQUIRE(f.manager.materialized(ticket, f.id(1), resources(7)));
    REQUIRE(f.manager.quiesced({lease, ContainmentProof::worker_quiesced, true}, 1ns));
    REQUIRE(f.manager.snapshot(f.device).executing.empty());
    REQUIRE(f.manager.snapshot(f.device).resident == resources(7));
    auto feasible = f.manager.dependency_feasible(f.device, resources(5, 1), resources(7));
    REQUIRE_FALSE(feasible);
    REQUIRE(feasible.error().reason() == ErrorReason::resource_deadlock);
    REQUIRE_FALSE(f.manager.reserve(f.request(2, 2, resources(5, 1)), 2ns));
}
TEST_CASE("A11 shared allocation counted once while each Run has a working-set quota",
          "[resources][A11]") {
    Fixture f;
    auto [t1, l1] = f.start(f.request(1, 1, resources(8, 1), resources(8, 1)));
    auto shared = f.id(1), private1 = f.id(2), private2 = f.id(3);
    REQUIRE(f.manager.materialized(t1, shared, resources(6)));
    REQUIRE(f.manager.materialized(t1, private1, resources(2)));
    auto r2 = f.request(2, 2, resources(2, 1), resources(8, 1));
    r2.shared = {shared};
    auto [t2, l2] = f.start(r2);
    REQUIRE(f.manager.materialized(t2, private2, resources(2)));
    REQUIRE(f.manager.snapshot(f.device).resident == resources(10));
    auto r3 = f.request(3, 3, resources(0, 0), resources(5, 1));
    r3.shared = {shared};
    auto rejected = f.manager.reserve(r3, 0ns);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code() == ErrorCode::budget_exceeded);
    REQUIRE(f.manager.quiesced({l1, ContainmentProof::worker_quiesced, true}));
    REQUIRE(f.manager.drop_reference(shared, t1.owner.job));
    REQUIRE_FALSE(f.manager.request_release(shared, OperationId{20}));
    REQUIRE(f.manager.drop_reference(private1, t1.owner.job));
    REQUIRE(f.manager.request_release(private1, OperationId{21}));
    REQUIRE(f.manager.release_confirmed({private1, OperationId{21}, true}, 1ns));
    REQUIRE(f.manager.snapshot(f.device).resident == resources(8));
    REQUIRE(f.manager.drop_reference(shared, t2.owner.job));
    REQUIRE(f.manager.request_release(shared, OperationId{20}));
    REQUIRE(f.manager.snapshot(f.device).resident == resources(8));
    REQUIRE(f.manager.release_confirmed({shared, OperationId{20}, true}, 2ns));
    REQUIRE(f.manager.snapshot(f.device).resident == resources(2));
}
TEST_CASE("A12 offload target and staging coexist until independent release receipts",
          "[resources][A12]") {
    Fixture f;
    auto [source_ticket, source_lease] = f.start(f.request(1, 1, resources(7, 1)));
    auto source = f.id(1);
    REQUIRE(f.manager.materialized(source_ticket, source, resources(7)));
    REQUIRE(f.manager.quiesced({source_lease, ContainmentProof::worker_quiesced, true}));
    auto [copy_ticket, copy_lease] = f.start(f.request(2, 1, resources(0, 1, 7, 2)));
    auto target = f.id(2), staging = f.id(3);
    REQUIRE(f.manager.materialized(copy_ticket, target, resources(0, 0, 7)));
    REQUIRE(f.manager.materialized(copy_ticket, staging, resources(0, 0, 0, 2)));
    REQUIRE(f.manager.snapshot(f.device).resident == resources(7, 0, 7, 2));
    REQUIRE(f.manager.drop_reference(source, source_ticket.owner.job));
    REQUIRE(f.manager.request_release(source, OperationId{10}));
    REQUIRE_FALSE(f.manager.reserve(f.request(3, 2, resources(5, 1)), 3ns));
    REQUIRE(f.manager.release_confirmed({source, OperationId{10}, true}, 4ns).value());
    REQUIRE_FALSE(f.manager.release_confirmed({source, OperationId{10}, true}, 5ns).value());
    REQUIRE(f.manager.snapshot(f.device).resident == resources(0, 0, 7, 2));
    REQUIRE(f.manager.reserve(f.request(4, 2, resources(5, 1)), 5ns));
    // A corrupt target is never made a compatible replacement by an acknowledgement for the source.
    REQUIRE_FALSE(
        f.manager.compatible({f.instance, AllocationId{2}, HostEpoch{1}, NativeWorkerGeneration{2}},
                             f.device, HostEpoch{1}, NativeWorkerGeneration{2}, 5ns));
}
TEST_CASE("A13 old full-identity release cannot mutate a replacement epoch", "[resources][A13]") {
    Fixture f;
    auto [t1, l1] = f.start(f.request(1, 1, resources(4, 1)));
    REQUIRE(f.manager.materialized(t1, f.id(1), resources(4)));
    f.manager.fence(f.device);
    REQUIRE_FALSE(f.manager.reserve(f.request(2, 2, resources(4, 1)), 1ns));
    auto new_envelope = f.envelope;
    new_envelope.epoch = HostEpoch{2};
    new_envelope.authority_revision = 2;
    new_envelope.reconciled_inventory = false;
    REQUIRE_FALSE(f.manager.observe_envelope(new_envelope, 1ns));
    new_envelope.reconciled_inventory = true;
    REQUIRE(f.manager.observe_envelope(new_envelope, 1ns));
    auto r2 = f.request(3, 2, resources(4, 1));
    r2.host_epoch = HostEpoch{2};
    r2.worker = NativeWorkerGeneration{2};
    auto [t2, l2] = f.start(r2);
    auto new_id = f.id(2, 2, 2);
    REQUIRE(f.manager.materialized(t2, new_id, resources(4)));
    REQUIRE(f.manager.drop_reference(f.id(1), t1.owner.job));
    REQUIRE(f.manager.request_release(f.id(1), OperationId{9}));
    REQUIRE_FALSE(f.manager.release_confirmed(
        {{f.instance, AllocationId{2}, HostEpoch{1}, NativeWorkerGeneration{1}},
         OperationId{9},
         true},
        2ns));
    REQUIRE(f.manager.release_confirmed({f.id(1), OperationId{9}, true}, 2ns));
    REQUIRE(f.manager.snapshot(f.device).resident == resources(4));
    REQUIRE(f.manager.ready_for_use(l2, 2ns));
    REQUIRE_FALSE(f.manager.ready_for_use(l1, 2ns));
}
TEST_CASE(
    "A28 A29 cleanup transfer requires containment and reserved observation; debt survives expiry",
    "[resources][A28][A29]") {
    Fixture f;
    auto [ticket, lease] = f.start(f.request(1, 1, resources(7, 1)));
    auto allocation = f.id(1);
    REQUIRE(f.manager.materialized(ticket, allocation, resources(7)));
    REQUIRE(f.manager.drop_reference(allocation, ticket.owner.job));
    REQUIRE(f.manager.request_release(allocation, OperationId{10}));
    CleanupTransfer transfer{allocation, lease, OperationId{10}, ContainmentProof::callback_fenced,
                             {},         100ns};
    REQUIRE_FALSE(f.manager.transfer_to_cleanup(transfer, 1ns));
    REQUIRE(f.manager.cleanup_count() == 0);
    transfer.containment = ContainmentProof::isolated_by_authority;
    f.observation.capacity = 0;
    REQUIRE_FALSE(f.manager.transfer_to_cleanup(transfer, 1ns));
    REQUIRE(f.manager.cleanup_count() == 0);
    f.observation.capacity = 1;
    auto cleanup = f.manager.transfer_to_cleanup(transfer, 1ns);
    REQUIRE(cleanup);
    REQUIRE(f.manager.transfer_to_cleanup(transfer, 1ns).value() == cleanup.value());
    REQUIRE(f.observation.reserved == 1);
    REQUIRE_FALSE(f.manager.lease_current(lease, 1ns));
    REQUIRE_FALSE(f.manager.next_allocation_identity(ticket));
    REQUIRE(f.manager.snapshot(f.device).quarantine == resources(7));
    REQUIRE(f.manager.snapshot(f.device).executing == resources(0, 1));
    REQUIRE_FALSE(f.manager.quiesced({lease, ContainmentProof::callback_fenced, true}));
    SECTION("both physical and execution release precede closure") {
        REQUIRE(f.manager.release_confirmed({allocation, OperationId{10}, true}, 2ns));
        REQUIRE(f.manager.cleanup_count() == 1);
        REQUIRE(f.observation.changes.empty());
        REQUIRE(f.manager.quiesced({lease, ContainmentProof::isolated_by_authority, true}, 3ns));
        REQUIRE(f.manager.cleanup_count() == 0);
        REQUIRE(f.observation.changes.size() == 2);
        REQUIRE(f.observation.changes.back().resolved);
        REQUIRE_FALSE(
            f.manager.release_confirmed({allocation, OperationId{10}, true}, 4ns).value());
        REQUIRE(f.observation.changes.size() == 2);
    }
    SECTION("stream expiry never refunds the ledger") {
        f.manager.close_observations(101ns);
        REQUIRE(f.observation.changes.size() == 1);
        REQUIRE_FALSE(f.observation.changes.back().resolved);
        REQUIRE(f.manager.snapshot(f.device).resident == resources(7));
        REQUIRE(f.manager.quiesced({lease, ContainmentProof::isolated_by_authority, true}, 102ns));
        REQUIRE(f.manager.release_confirmed({allocation, OperationId{10}, true}, 103ns));
        REQUIRE(f.manager.cleanup_count() == 0);
        REQUIRE(f.observation.changes.size() == 1);
        REQUIRE(f.manager.snapshot(f.device).resident.empty());
    }
}
TEST_CASE("C03 unknown acquisition and partial allocation never refund by timeout",
          "[resources][C03]") {
    Fixture f;
    auto ticket = f.manager.reserve(f.request(1, 1, resources(7, 1)), 0ns);
    REQUIRE(ticket);
    REQUIRE_FALSE(f.manager.abort_preparation(ticket.value(), false));
    REQUIRE(f.manager.snapshot(f.device).reserved == resources(7, 1));
    REQUIRE(f.host.reconciliations.size() == 1);
    REQUIRE(f.manager.acquired({ticket.value(), f.instance, OperationId{}, HostOutcome::rejected}));
    REQUIRE(f.manager.abort_preparation(ticket.value(), false));
    REQUIRE(f.manager.snapshot(f.device).reserved.empty());
    auto [t, l] = f.start(f.request(2, 1, resources(7, 1)));
    REQUIRE(f.manager.materialized(t, f.id(1), resources(3)));
    REQUIRE_FALSE(f.manager.quiesced({l, ContainmentProof::worker_quiesced, true}));
    REQUIRE(f.manager.snapshot(f.device).reserved == resources(4));
    REQUIRE(f.manager.quiesced({l, ContainmentProof::worker_quiesced, true, true}));
    REQUIRE(f.manager.snapshot(f.device).resident == resources(3));
}
TEST_CASE("C03 overflow rejection and reservation operation dedup leave ledger unchanged",
          "[resources][C03]") {
    Fixture f;
    auto request = f.request(1, 1, resources(4, 1));
    auto ticket = f.manager.reserve(request, 0ns);
    REQUIRE(ticket);
    REQUIRE(f.manager.reserve(request, 0ns).value() == ticket.value());
    REQUIRE(f.host.acquisitions.size() == 1);
    request.incremental = resources(5, 1);
    REQUIRE_FALSE(f.manager.reserve(request, 0ns));
    auto large = f.request(2, 2, resources(std::numeric_limits<std::uint64_t>::max(), 1));
    REQUIRE_FALSE(f.manager.reserve(large, 0ns));
    REQUIRE(f.manager.snapshot(f.device).reserved == resources(4, 1));
}
TEST_CASE("C03 a throwing host port retains accepted acquisition identity and liability",
          "[resources][C03][fault]") {
    Fixture f;
    f.host.throw_after_acquire = true;
    auto request = f.request(1, 1, resources(7, 1));
    auto result = f.manager.reserve(request, 0ns);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().external_outcome() == ExternalOutcome::unknown);
    REQUIRE(result.error().code() == ErrorCode::outcome_unknown);
    auto ticket = f.manager.ticket_for(OperationId{1});
    REQUIRE(ticket);
    REQUIRE(f.manager.snapshot(f.device).reserved == resources(7, 1));
    REQUIRE(f.manager.reserve(request, 1ns).value() == *ticket);
    REQUIRE(f.host.acquisitions.size() == 1);
    REQUIRE_FALSE(f.manager.commit_dispatch(*ticket, {true, true, true, true}, 1ns));
    f.host.throw_reconcile = true;
    REQUIRE_FALSE(f.manager.abort_preparation(*ticket, false));
    REQUIRE(f.manager.snapshot(f.device).reserved == resources(7, 1));
}
TEST_CASE("C03 settled slots retire without refunding uncertainty or reusing allocation identities",
          "[resources][C03][retention]") {
    RuntimeInstanceId instance{1, 2};
    ManualHost host;
    ManualCleanupObservation observations;
    ResourceManagerLimits limits;
    limits.reservations = 1;
    limits.allocations = 2;
    ResourceManager manager{instance, host, observations, limits};
    REQUIRE(manager.observe_envelope({LogicalResourceId{1}, instance, HostEpoch{1}, 1, 1000ns,
                                      resources(10, 1), HostArbitration::enforced_generation, true,
                                      true},
                                     0ns));
    std::optional<ReleaseEvidence> old;
    for (std::uint64_t i = 1; i <= 4; ++i) {
        ResourceRequest request{OperationId{i},
                                {JobId{RunId{instance, i}, 1}, AttemptId{1}, DispatchGeneration{1}},
                                LogicalResourceId{1},
                                HostEpoch{1},
                                NativeWorkerGeneration{1},
                                resources(2, 1),
                                resources(2, 1),
                                900ns,
                                {}};
        auto ticket = manager.reserve(request, 0ns);
        REQUIRE(ticket);
        REQUIRE(manager.acquired(
            {ticket.value(), instance, OperationId{100 + i}, HostOutcome::acknowledged}));
        auto lease = manager.commit_dispatch(ticket.value(), {true, true, true, true}, 0ns);
        REQUIRE(lease);
        auto first = manager.next_allocation_identity(ticket.value()),
             second = manager.next_allocation_identity(ticket.value());
        REQUIRE(first);
        REQUIRE(second);
        REQUIRE(manager.materialized(ticket.value(), second.value(), resources(1)));
        REQUIRE(manager.materialized(ticket.value(), first.value(), resources(1)));
        if (old)
            REQUIRE_FALSE(manager.release_confirmed(*old, 0ns));
        REQUIRE(manager.retire_settled() == 0);
        REQUIRE(manager.snapshot(LogicalResourceId{1}).resident == resources(2));
        REQUIRE(manager.quiesced({lease.value(), ContainmentProof::worker_quiesced, true}));
        for (auto id : {first.value(), second.value()}) {
            REQUIRE(manager.drop_reference(id, ticket.value().owner.job));
            REQUIRE(manager.request_release(id, OperationId{1000 + id.allocation.value()}));
            ReleaseEvidence release{id, OperationId{1000 + id.allocation.value()}, true};
            REQUIRE(manager.release_confirmed(release, 1ns));
            old = release;
        }
        REQUIRE(manager.retire_settled() == 3);
        REQUIRE(manager.snapshot(LogicalResourceId{1}).resident.empty());
        REQUIRE_FALSE(manager.materialized(ticket.value(), first.value(), resources(1)));
    }
}
TEST_CASE("A23 A29 unknown acquisition debt transfers without a fabricated allocation",
          "[resources][A23][A29]") {
    Fixture f;
    auto ticket = f.manager.reserve(f.request(1, 1, resources(7, 1)), 0ns);
    REQUIRE(ticket);
    REQUIRE_FALSE(f.manager.transfer_acquisition_to_cleanup(
        ticket.value(), ContainmentProof::callback_fenced, 100ns, 1ns));
    auto cleanup = f.manager.transfer_acquisition_to_cleanup(
        ticket.value(), ContainmentProof::isolated_by_authority, 100ns, 1ns);
    REQUIRE(cleanup);
    REQUIRE(f.manager.cleanup_count() == 1);
    REQUIRE(f.manager.snapshot(f.device).quarantine == resources(7, 1));
    REQUIRE(f.manager.retire_settled() == 0);
    REQUIRE_FALSE(f.manager.commit_dispatch(ticket.value(), {true, true, true, true}, 2ns));
    REQUIRE_FALSE(f.manager.abort_preparation(ticket.value(), false, 2ns));
    REQUIRE(f.manager.snapshot(f.device).reserved == resources(7, 1));
    f.manager.close_observations(101ns);
    REQUIRE(f.observation.changes.size() == 1);
    REQUIRE(f.manager.acquired({ticket.value(), f.instance, OperationId{}, HostOutcome::rejected}));
    REQUIRE(f.manager.abort_preparation(ticket.value(), false, 102ns));
    REQUIRE(f.manager.snapshot(f.device).reserved.empty());
    REQUIRE(f.manager.cleanup_count() == 0);
    REQUIRE(f.observation.changes.size() == 1);
    REQUIRE(f.manager.retire_settled() == 2);
}
TEST_CASE("A28 in-flight native work without reported allocation retains reservation debt",
          "[resources][A28]") {
    Fixture f;
    auto [ticket, lease] = f.start(f.request(1, 1, resources(7, 1)));
    REQUIRE(f.manager.transfer_acquisition_to_cleanup(
        ticket, ContainmentProof::isolated_by_authority, 100ns, 1ns));
    REQUIRE_FALSE(f.manager.lease_current(lease, 1ns));
    REQUIRE_FALSE(f.manager.quiesced({lease, ContainmentProof::callback_fenced, true, true}, 2ns));
    REQUIRE(f.manager.snapshot(f.device).quarantine == resources(7, 1));
    REQUIRE(f.manager.quiesced({lease, ContainmentProof::isolated_by_authority, true, true}, 3ns));
    REQUIRE(f.manager.cleanup_count() == 0);
    REQUIRE(f.manager.snapshot(f.device).reserved.empty());
    REQUIRE(f.manager.snapshot(f.device).executing.empty());
    REQUIRE(f.observation.changes.size() == 2);
    REQUIRE(f.observation.changes.back().resolved);
}
TEST_CASE("C03 complete allocation manifest retains conservative scratch headroom",
          "[resources][manifest]") {
    Fixture f;
    auto [ticket, lease] = f.start(f.request(1, 1, resources(10, 1)));
    auto first = f.manager.next_allocation_identity(ticket),
         second = f.manager.next_allocation_identity(ticket);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(f.manager.materialized(ticket, first.value(), resources(3)));
    std::vector<AllocationIdentity> missing{first.value()};
    REQUIRE_FALSE(f.manager.materialization_complete(ticket, missing, 0ns));
    REQUIRE_FALSE(f.manager.ready_for_use(lease, 0ns));
    REQUIRE(f.manager.materialized(ticket, second.value(), resources(2)));
    std::vector<AllocationIdentity> manifest{second.value(), first.value()};
    SECTION("missing, duplicate, and foreign identities cannot certify initial materialization") {
        REQUIRE_FALSE(f.manager.materialization_complete(ticket, missing, 0ns));
        manifest = {first.value(), first.value()};
        REQUIRE_FALSE(f.manager.materialization_complete(ticket, manifest, 0ns));
        manifest = {first.value(), f.id(100)};
        REQUIRE_FALSE(f.manager.materialization_complete(ticket, manifest, 0ns));
        REQUIRE(f.manager.snapshot(f.device).reserved == resources(5));
        REQUIRE_FALSE(f.manager.ready_for_use(lease, 0ns));
    }
    SECTION("the manifest completes readiness while upper-bound memory remains charged") {
        REQUIRE(f.manager.materialization_complete(ticket, manifest, 0ns));
        REQUIRE(f.manager.ready_for_use(lease, 0ns));
        REQUIRE(f.manager.snapshot(f.device).resident == resources(5));
        REQUIRE(f.manager.snapshot(f.device).reserved == resources(5));
        REQUIRE_FALSE(f.manager.reserve(f.request(2, 2, resources(1, 1)), 0ns));
        REQUIRE(f.manager.materialization_complete(ticket, manifest, 0ns));
        auto scratch = f.manager.next_allocation_identity(ticket);
        REQUIRE(scratch);
        REQUIRE(f.manager.materialized(ticket, scratch.value(), resources(1)));
        REQUIRE(f.manager.ready_for_use(lease, 0ns));
        REQUIRE(f.manager.snapshot(f.device).resident == resources(6));
        REQUIRE(f.manager.snapshot(f.device).reserved == resources(4));
        REQUIRE_FALSE(f.manager.materialized(ticket, f.id(100), resources(5)));
        f.manager.fence(f.device);
        REQUIRE_FALSE(f.manager.materialization_complete(ticket, manifest, 1ns));
        REQUIRE_FALSE(f.manager.ready_for_use(lease, 1ns));
    }
}
TEST_CASE("C03 ledger owns release correlation across throwing host handoff and retired Run",
          "[resources][host-release][fault]") {
    Fixture f;
    auto [ticket, lease] = f.start(f.request(1, 1, resources(7, 1)));
    REQUIRE(f.manager.materialized(ticket, f.id(1), resources(3)));
    std::array manifest{f.id(1)};
    REQUIRE(f.manager.materialization_complete(ticket, manifest, 0ns));
    REQUIRE(f.manager.native_quiesced(ticket, ContainmentProof::worker_quiesced, true, 1ns));
    REQUIRE_FALSE(f.manager.reservation_settled(ticket));
    REQUIRE(f.manager.snapshot(f.device).reserved == resources(4));
    f.host.throw_after_release = true;
    auto sent = f.manager.request_host_release(ticket, OperationId{10});
    REQUIRE_FALSE(sent);
    REQUIRE(sent.error().external_outcome() == ExternalOutcome::unknown);
    REQUIRE(f.manager.host_release_operation(ticket) == OperationId{10});
    REQUIRE(f.manager.request_host_release(ticket, OperationId{10}));
    REQUIRE(f.host.releases.size() == 1);
    REQUIRE_FALSE(f.manager.request_host_release(ticket, OperationId{11}));
    REQUIRE_FALSE(f.manager.observe_host_release({OperationId{11}, ticket, lease.host_grant}, 2ns));
    REQUIRE_FALSE(f.manager.observe_host_release({OperationId{10}, ticket, OperationId{999}}, 2ns));
    REQUIRE(f.manager.observe_host_release({OperationId{10}, ticket, lease.host_grant}, 3ns));
    REQUIRE(f.manager.reservation_settled(ticket));
    REQUIRE(f.manager.lease_quiescent(lease));
    REQUIRE(f.manager.snapshot(f.device).executing.empty());
    REQUIRE(f.manager.snapshot(f.device).reserved.empty());
    REQUIRE(f.manager.snapshot(f.device).resident == resources(3));
    REQUIRE(f.manager.observe_host_release({OperationId{10}, ticket, lease.host_grant}, 4ns));
    REQUIRE(f.host.releases.size() == 1);
}
TEST_CASE("C03 acquired preparation can release before any Job dispatch",
          "[resources][host-release]") {
    Fixture f;
    auto ticket = f.manager.reserve(f.request(1, 1, resources(7, 1)), 0ns);
    REQUIRE(ticket);
    REQUIRE(f.manager.acquired(
        {ticket.value(), f.instance, OperationId{2}, HostOutcome::acknowledged}));
    REQUIRE(f.manager.request_host_release(ticket.value(), OperationId{3}));
    REQUIRE(f.manager.observe_host_release({OperationId{3}, ticket.value(), OperationId{2}}, 1ns));
    REQUIRE_FALSE(f.manager.reservation_settled(ticket.value()));
    REQUIRE(
        f.manager.native_quiesced(ticket.value(), ContainmentProof::worker_quiesced, true, 2ns));
    REQUIRE(f.manager.reservation_settled(ticket.value()));
    REQUIRE(f.manager.snapshot(f.device).reserved.empty());
    REQUIRE(f.manager.retire_settled() == 1);
    REQUIRE_FALSE(
        f.manager.observe_host_release({OperationId{3}, ticket.value(), OperationId{2}}, 3ns));
}
