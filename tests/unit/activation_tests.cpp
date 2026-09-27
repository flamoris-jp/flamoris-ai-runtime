#include "../support/manual_activation_gateway.hpp"
#include "../support/manual_resource_ports.hpp"
#include "flamoris/runtime/activation.hpp"
#include <catch_amalgamated.hpp>
using namespace flamoris::runtime;
using namespace flamoris::runtime::test;
using namespace std::chrono_literals;
namespace {
ActivationKey key() { return {"runtime.native", 1, 1}; }
ActivationRequest trigger(std::uint64_t id) {
    return {OperationId{id}, key(), HostEpoch{1}, 100ns};
}
struct ActivationFixture {
    ManualActivationPolicy policy;
    ManualActivationGateway gateway{policy};
    ActivationClient client{gateway, policy};
    void deliver(TimePoint now = 0ns) {
        auto observations = std::move(gateway.observations);
        gateway.observations.clear();
        gateway.observations.reserve(128);
        for (const auto &observation : observations)
            REQUIRE(client.observe(observation, now));
    }
};
ResourceVector vec(std::uint64_t memory, std::uint64_t slots = 0) {
    ResourceVector v;
    v[ResourceKind::device] = memory;
    v[ResourceKind::execution] = slots;
    return v;
}
struct ModelFixture {
    RuntimeInstanceId instance{3, 4};
    ManualHost host;
    ManualCleanupObservation observations;
    ResourceManager resources{instance, host, observations};
    ModelLoadRegistry registry{resources};
    ModelResidencyKey key{{}, LogicalResourceId{1}, HostEpoch{1}, NativeWorkerGeneration{1}};
    ResourceOwner owner{JobId{RunId{instance, 1}, 1}, AttemptId{1}, DispatchGeneration{1}};
    ResourceTicket ticket;
    ExecutionLease lease;
    AllocationIdentity allocation{instance, AllocationId{1}, HostEpoch{1},
                                  NativeWorkerGeneration{1}};
    ModelFixture() {
        REQUIRE(resources.observe_envelope({key.resource, instance, key.host_epoch, 1, 100ns,
                                            vec(10, 1), HostArbitration::enforced_generation, true,
                                            true},
                                           0ns));
        ResourceRequest request{OperationId{1}, owner,      key.resource,
                                key.host_epoch, key.worker, vec(6, 1),
                                vec(8, 1),      90ns,       {}};
        auto t = resources.reserve(request, 0ns);
        REQUIRE(t);
        ticket = t.value();
        REQUIRE(resources.acquired({ticket, instance, OperationId{2}, HostOutcome::acknowledged}));
        auto l = resources.commit_dispatch(ticket, {true, true, true, true}, 0ns);
        REQUIRE(l);
        lease = l.value();
    }
};
} // namespace
TEST_CASE("B-ACT01 compatible cold waiters share one external host start and revisions conflict",
          "[activation][B-ACT01]") {
    ActivationFixture f;
    REQUIRE(f.client.request(trigger(1), 0ns));
    REQUIRE(f.client.request(trigger(2), 0ns));
    auto conflict = trigger(3);
    conflict.key.configuration_revision = 2;
    REQUIRE_FALSE(f.client.request(conflict, 0ns));
    REQUIRE(f.gateway.records.size() == 1);
    REQUIRE(f.gateway.host_starts == 0);
    f.deliver();
    REQUIRE(f.gateway.perform_start(0, 1ns));
    REQUIRE_FALSE(f.gateway.perform_start(0, 1ns));
    REQUIRE(f.gateway.host_starts == 1);
    f.gateway.ready(0, {9, 1});
    f.deliver(2ns);
    auto one = f.client.inspect(OperationId{1}), two = f.client.inspect(OperationId{2});
    REQUIRE(one);
    REQUIRE(two);
    REQUIRE(one.value().generation == two.value().generation);
    REQUIRE(one.value().instance == two.value().instance);
    REQUIRE(f.gateway.host_stops == 0);
}
TEST_CASE("B-ACT02 disconnecting all waiters does not cancel the shared process",
          "[activation][B-ACT02]") {
    ActivationFixture f;
    REQUIRE(f.client.request(trigger(1), 0ns));
    REQUIRE(f.client.request(trigger(2), 0ns));
    f.deliver();
    REQUIRE(f.client.detach(OperationId{1}));
    REQUIRE(f.client.detach(OperationId{1}));
    REQUIRE(f.client.detach(OperationId{2}));
    REQUIRE(f.gateway.records[0].waiters.empty());
    REQUIRE(f.gateway.host_stops == 0);
    REQUIRE(f.gateway.perform_start(0, 1ns));
    f.gateway.ready(0, {9, 1});
    REQUIRE(f.gateway.records[0].status == ProcessStatus::ready);
    REQUIRE(f.gateway.host_starts == 1);
    REQUIRE(f.gateway.observations.empty());
}
TEST_CASE(
    "B-ACT03 partial startup failure or deadline blocks replacement until host reconciliation",
    "[activation][B-ACT03]") {
    ActivationFixture f;
    REQUIRE(f.client.request(trigger(1), 0ns));
    f.deliver();
    REQUIRE(f.gateway.perform_start(0, 1ns));
    f.gateway.partial_failure(0);
    f.deliver(2ns);
    REQUIRE(f.gateway.records[0].uncertain_resources);
    REQUIRE_FALSE(f.client.request(trigger(2), 2ns));
    REQUIRE(f.gateway.host_starts == 1);
    f.client.expire(101ns);
    REQUIRE(f.client.inspect(OperationId{1}).value().status == ProcessStatus::unknown);
    f.gateway.stop_confirmed(0);
    auto retry = trigger(3);
    retry.deadline = 200ns;
    REQUIRE(f.client.request(retry, 102ns));
    REQUIRE(f.gateway.records[0].generation == ActivationGeneration{2});
    REQUIRE_FALSE(f.gateway.records[0].uncertain_resources);
}
TEST_CASE("B-ACT05 B-ACT08 stale startup and stop receipts cannot change replacement readiness",
          "[activation][B-ACT05][B-ACT08]") {
    ActivationFixture f;
    REQUIRE(f.client.request(trigger(1), 0ns));
    f.deliver();
    REQUIRE(f.gateway.perform_start(0, 1ns));
    ActivationObservation stale{OperationId{1},
                                key(),
                                ActivationGeneration{1},
                                HostEpoch{1},
                                ProcessStatus::ready,
                                {9, 1},
                                1,
                                false};
    f.client.host_epoch_lost(HostEpoch{1});
    f.gateway.lose_epoch();
    REQUIRE_FALSE(f.client.observe(stale, 2ns));
    auto unknown = f.client.inspect(OperationId{1});
    REQUIRE(unknown.value().status == ProcessStatus::unknown);
    auto next = trigger(2);
    next.expected_epoch = HostEpoch{2};
    REQUIRE_FALSE(f.client.request(next, 2ns));
    f.gateway.stop_confirmed(0);
    f.gateway.observations.clear();
    next.request = OperationId{3};
    REQUIRE(f.client.request(next, 3ns));
    f.deliver(3ns);
    REQUIRE(f.gateway.perform_start(0, 4ns));
    f.gateway.ready(0, {9, 2});
    f.deliver(5ns);
    auto current = f.client.inspect(OperationId{3});
    REQUIRE(current.value().instance == RuntimeInstanceId{9, 2});
    stale.request = OperationId{3};
    REQUIRE_FALSE(f.client.observe(stale, 6ns));
    stale.status = ProcessStatus::stopped;
    stale.safe_stop_confirmed = true;
    REQUIRE_FALSE(f.client.observe(stale, 6ns));
    REQUIRE(f.client.observe(current.value(), 6ns));
    REQUIRE(f.client.inspect(OperationId{3}).value() == current.value());
}
TEST_CASE("B-ACT06 idle admission gate linearizes new claims against closure",
          "[activation][B-ACT06]") {
    AdmissionGate gate;
    auto claim = gate.claim();
    REQUIRE(claim);
    REQUIRE_FALSE(gate.prepare_idle_stop(1, {}));
    REQUIRE(gate.settle_claim(claim.value()));
    IdleSnapshot active;
    active.model_loads = 1;
    REQUIRE_FALSE(gate.prepare_idle_stop(1, active));
    REQUIRE(gate.prepare_idle_stop(1, {}));
    REQUIRE(gate.state() == AdmissionGateState::draining);
    REQUIRE_FALSE(gate.claim());
    REQUIRE_FALSE(gate.shutdown_confirmed(ContainmentProof::callback_fenced));
    REQUIRE(gate.shutdown_confirmed(ContainmentProof::worker_quiesced));
    REQUIRE(gate.state() == AdmissionGateState::closed);
    REQUIRE_FALSE(gate.claim());
    AdmissionGate closure_first;
    REQUIRE(closure_first.prepare_idle_stop(1, {}));
    REQUIRE_FALSE(closure_first.claim());
}
TEST_CASE("B-ACT09 activation model inspect and stop permissions are independent and current",
          "[activation][B-ACT09]") {
    ActivationFixture f;
    f.policy.allowed[0] = false;
    REQUIRE_FALSE(f.client.request(trigger(1), 0ns));
    REQUIRE(f.gateway.records.empty());
    f.policy.allowed[0] = true;
    REQUIRE(f.client.request(trigger(1), 0ns));
    f.deliver();
    f.policy.allowed[0] = false;
    REQUIRE_FALSE(f.gateway.perform_start(0, 1ns));
    REQUIRE(f.gateway.host_starts == 0);
    ActivationFixture allowed;
    REQUIRE(allowed.client.request(trigger(1), 0ns));
    allowed.deliver();
    REQUIRE(allowed.gateway.perform_start(0, 1ns));
    allowed.gateway.ready(0, {9, 1});
    allowed.deliver(2ns);
    allowed.policy.allowed[1] = false;
    REQUIRE_FALSE(allowed.client.inspect(OperationId{1}));
    allowed.policy.allowed[3] = false;
    REQUIRE_FALSE(allowed.client.authorize_model(OperationId{1}));
    allowed.policy.allowed[2] = false;
    REQUIRE_FALSE(allowed.client.request_stop(OperationId{1}, 50ns, 2ns));
    REQUIRE(allowed.gateway.host_stops == 0);
    UnavailableActivationGateway cold;
    ActivationClient absent{cold, allowed.policy};
    REQUIRE_FALSE(absent.request(trigger(2), 2ns));
}
TEST_CASE("B-ACT10 ready process does not grant incomplete resource inventory",
          "[activation][B-ACT10]") {
    ActivationFixture f;
    REQUIRE(f.client.request(trigger(1), 0ns));
    f.deliver();
    REQUIRE(f.gateway.perform_start(0, 1ns));
    f.gateway.ready(0, {9, 2});
    f.deliver(2ns);
    ManualHost host;
    ManualCleanupObservation observations;
    ResourceManager manager{{9, 2}, host, observations};
    ResourceRequest request{OperationId{1},
                            {JobId{RunId{{9, 2}, 1}, 1}, AttemptId{1}, DispatchGeneration{1}},
                            LogicalResourceId{1},
                            HostEpoch{1},
                            NativeWorkerGeneration{1},
                            vec(1, 1),
                            vec(10, 1),
                            90ns,
                            {}};
    REQUIRE_FALSE(manager.reserve(request, 2ns));
    REQUIRE(host.acquisitions.empty());
    HostEnvelope independent{
        {2},  {9, 2}, HostEpoch{1}, 1, 100ns, vec(10, 1), HostArbitration::enforced_single_owner,
        true, true};
    REQUIRE(manager.observe_envelope(independent, 2ns));
    request.resource = {2};
    REQUIRE(manager.reserve(request, 2ns));
}
TEST_CASE("B-ACT11 lost forwarded admission cannot replay unkeyed work on a new incarnation",
          "[activation][B-ACT11]") {
    RuntimeInstanceId old{1, 1}, fresh{1, 2};
    REQUIRE_FALSE(may_retry_submission(ForwardedAdmission::unknown, old, fresh, true));
    REQUIRE_FALSE(may_retry_submission(ForwardedAdmission::unknown, old, old, false));
    REQUIRE(may_retry_submission(ForwardedAdmission::unknown, old, old, true));
    REQUIRE(may_retry_submission(ForwardedAdmission::rejected_before_run, old, fresh, false));
    REQUIRE_FALSE(may_retry_submission(ForwardedAdmission::admitted, old, fresh, true));
}
TEST_CASE("B-ACT04 cold model has one owning Job and queued contenders hold no lease",
          "[activation][B-ACT04]") {
    ModelFixture f;
    REQUIRE(f.registry.availability(f.key) == ModelLoadDecision::load_owned);
    REQUIRE(f.registry.begin(f.key, f.owner, f.lease, false, 90ns, 0ns));
    REQUIRE(f.registry.availability(f.key) == ModelLoadDecision::wait_without_lease);
    REQUIRE_FALSE(f.registry.begin(f.key, f.owner, f.lease, false, 90ns, 0ns));
    REQUIRE(f.host.acquisitions.size() == 1);
    REQUIRE(f.resources.snapshot(f.key.resource).executing == vec(0, 1));
    REQUIRE_FALSE(f.registry.completed(f.key, f.owner, f.allocation, 1ns));
    REQUIRE(f.resources.materialized(f.ticket, f.allocation, vec(6)));
    REQUIRE(f.registry.completed(f.key, f.owner, f.allocation, 2ns));
    REQUIRE(f.registry.availability(f.key) == ModelLoadDecision::resident_available);
    REQUIRE(f.resources.snapshot(f.key.resource).resident == vec(6));
    REQUIRE(f.registry.completed(f.key, f.owner, f.allocation, 2ns));
}
TEST_CASE("B-ACT07 load cancellation never transfers owner or accepts a late resident result",
          "[activation][B-ACT07]") {
    ModelFixture f;
    REQUIRE(f.registry.begin(f.key, f.owner, f.lease, false, 90ns, 0ns));
    SECTION("cancel first preserves partial allocation and blocks replacement") {
        REQUIRE(f.registry.cancel(f.key, f.owner));
        REQUIRE(f.resources.materialized(f.ticket, f.allocation, vec(6)));
        REQUIRE_FALSE(f.registry.completed(f.key, f.owner, f.allocation, 2ns));
        REQUIRE(f.registry.availability(f.key) == ModelLoadDecision::wait_without_lease);
        REQUIRE_FALSE(
            f.registry.cleanup_settled(f.key, f.owner, ContainmentProof::callback_fenced));
        REQUIRE_FALSE(
            f.registry.cleanup_settled(f.key, f.owner, ContainmentProof::worker_quiesced));
        REQUIRE(f.resources.snapshot(f.key.resource).resident == vec(6));
    }
    SECTION("completion first retains shared cache but removes cancelled owner") {
        REQUIRE(f.resources.materialized(f.ticket, f.allocation, vec(6)));
        REQUIRE(f.registry.completed(f.key, f.owner, f.allocation, 1ns));
        REQUIRE(f.registry.cancel(f.key, f.owner));
        REQUIRE(f.registry.availability(f.key) == ModelLoadDecision::resident_available);
        REQUIRE(f.resources.allocation(f.allocation)->references == 0);
        REQUIRE(f.resources.snapshot(f.key.resource).resident == vec(6));
    }
}
TEST_CASE("B-ACT12 nonpausable and stuck native loaders keep ownership and capacity",
          "[activation][B-ACT12]") {
    ModelFixture f;
    REQUIRE(f.registry.begin(f.key, f.owner, f.lease, false, 90ns, 0ns));
    REQUIRE_FALSE(f.registry.can_pause(f.key));
    REQUIRE(f.registry.cancel(f.key, f.owner));
    REQUIRE_FALSE(f.registry.cleanup_settled(f.key, f.owner, ContainmentProof::callback_fenced));
    REQUIRE_FALSE(
        f.resources.quiesced({f.lease, ContainmentProof::callback_fenced, true, true}, 100ns));
    REQUIRE(f.resources.snapshot(f.key.resource).reserved == vec(6));
    REQUIRE(f.resources.snapshot(f.key.resource).executing == vec(0, 1));
}
TEST_CASE("C06 throwing gateway preserves unknown request and never repeats process activation",
          "[activation][C06][fault]") {
    ActivationFixture f;
    f.gateway.throw_after_request = true;
    auto result = f.client.request(trigger(1), 0ns);
    REQUIRE_FALSE(result);
    REQUIRE(result.error().external_outcome() == ExternalOutcome::unknown);
    REQUIRE(f.client.inspect(OperationId{1}).value().status == ProcessStatus::unknown);
    REQUIRE(f.client.request(trigger(1), 1ns));
    REQUIRE(f.gateway.records.size() == 1);
    REQUIRE(f.gateway.records[0].waiters.size() == 1);
    f.policy.throw_on_check = true;
    REQUIRE_FALSE(f.client.request(trigger(2), 1ns));
    REQUIRE(f.gateway.records[0].waiters.size() == 1);
    REQUIRE(f.gateway.host_starts == 0);
}
TEST_CASE("C06 detached receipt slots can retire while the host retains shared ownership",
          "[activation][C06][retention]") {
    ManualActivationPolicy policy;
    ManualActivationGateway gateway{policy};
    ActivationClient client{gateway, policy, 1};
    REQUIRE(client.request(trigger(1), 0ns));
    REQUIRE(client.detach(OperationId{1}));
    REQUIRE(client.retire_detached() == 1);
    REQUIRE_FALSE(client.request(trigger(1), 1ns));
    REQUIRE(client.request(trigger(2), 1ns));
    REQUIRE(gateway.records.size() == 1);
    REQUIRE(gateway.records[0].waiters.size() == 1);
    REQUIRE(gateway.host_stops == 0);
    REQUIRE_FALSE(client.observe(gateway.observations.front(), 1ns));
}
TEST_CASE("B-ACT02 B-ACT03 late waiter cancellation and host startup deadline remain separate",
          "[activation][B-ACT02][B-ACT03]") {
    ActivationFixture f;
    REQUIRE(f.client.request(trigger(1), 0ns));
    f.deliver();
    REQUIRE(f.gateway.perform_start(0, 1ns));
    SECTION("ready process survives its last waiter") {
        REQUIRE(f.gateway.ready(0, {9, 1}, 2ns));
        f.deliver(2ns);
        REQUIRE(f.client.detach(OperationId{1}));
        REQUIRE(f.gateway.records[0].status == ProcessStatus::ready);
        REQUIRE(f.gateway.host_stops == 0);
    }
    SECTION("partial startup remains unknown after its host deadline") {
        f.gateway.expire(101ns);
        f.deliver(101ns);
        REQUIRE(f.gateway.records[0].uncertain_resources);
        REQUIRE_FALSE(f.gateway.ready(0, {9, 1}, 102ns));
        auto request = trigger(2);
        request.deadline = 200ns;
        REQUIRE_FALSE(f.client.request(request, 102ns));
        REQUIRE(f.gateway.host_starts == 1);
    }
}
TEST_CASE("B-ACT05 model load cannot publish resident state after native epoch loss",
          "[activation][B-ACT05]") {
    ModelFixture f;
    REQUIRE(f.registry.begin(f.key, f.owner, f.lease, false, 90ns, 0ns));
    REQUIRE(f.resources.materialized(f.ticket, f.allocation, vec(6)));
    f.resources.fence(f.key.resource);
    f.registry.fence(f.key.host_epoch);
    REQUIRE_FALSE(f.registry.completed(f.key, f.owner, f.allocation, 1ns));
    REQUIRE(f.registry.availability(f.key) == ModelLoadDecision::wait_without_lease);
    REQUIRE(f.resources.snapshot(f.key.resource).resident == vec(6));
}
