#include "catch_amalgamated.hpp"
#include "flamoris/runtime/cleanup_observation.hpp"
#include "flamoris/runtime/control_mailbox.hpp"
#include "flamoris/runtime/observation.hpp"
#include "flamoris/runtime/registered_adapter.hpp"
#include "support/deterministic.hpp"
#include "support/manual_resource_ports.hpp"

using namespace flamoris::runtime;
using namespace std::chrono_literals;
namespace {
struct Lookup final : RunObservationLookupPort {
    RunController *controller{};
    RunController *find_observation_run(RunId id) noexcept override {
        return controller && controller->id() == id ? controller : nullptr;
    }
};
struct ExternalWrite final : RegisteredProviderPort {
    unsigned calls{0}, queries{0};
    bool accepted{false};
    ExternalOutcome invoke(std::string_view, const JsonValue &, std::uint64_t,
                           BoundedProviderSink &) override {
        ++calls;
        accepted = true;
        throw std::runtime_error("private provider response lost after acceptance");
    }
    ExternalOutcome reconcile(std::string_view, std::uint64_t) override {
        ++queries;
        return ExternalOutcome::unknown;
    }
};
ResourceVector capacity() {
    ResourceVector value;
    value[ResourceKind::device] = 4;
    value[ResourceKind::execution] = 1;
    return value;
}
CapabilityContract capability() {
    CapabilityContract contract(EffectSet::from_names({"write", "external"}).value());
    contract.identifier = "service.write";
    contract.version = "1";
    contract.adapter_revision = "registered/1";
    contract.input_schema.kind = ValueSchema::Kind::object;
    contract.output_schema.kind = ValueSchema::Kind::object;
    return contract;
}
} // namespace

TEST_CASE("A33 abrupt component lifetime preserves external uncertainty and never restores work "
          "from trace") {
    // Discard production components without calling RuntimeInstance::shutdown or
    // synthesizing stop/release/terminal events. The external host/provider seam
    // survives; this fixture models process loss, not a graceful destructor test.
    for (unsigned crash_stage = 0; crash_stage < 3; ++crash_stage) {
        INFO(crash_stage);
        testing::ManualClock clock;
        const RuntimeInstanceId old_instance{211, 1}, new_instance{211, 2};
        const RunId old_run{old_instance, 1}, new_run{new_instance, 1};
        test::ManualHost host;
        ExternalWrite provider;
        const auto contract = capability();
        CapabilitySnapshot registry;
        registry.capabilities.emplace(contract.identifier, contract);
        ExecutionPlan plan;
        const auto pin = fingerprint_capability(contract).value();
        plan.pins.push_back(pin);
        AuthorizationContext context;
        context.subject = "owner";
        context.expires_at_ms = 100;
        context.capabilities = {contract.identifier};
        context.permitted_effects = 63;
        PolicySnapshot policy;
        policy.capabilities = context.capabilities;
        policy.permitted_effects = 63;
        AuthorizationRequest auth;
        auth.capability = contract.identifier;
        auth.deadline_ms = 90;
        auth.concrete_inputs = {};
        auth.concrete_input_digest =
            domain_digest("flamoris.operation-input/1\n", JsonValue::Object{}).value();
        std::vector<EventGroup> retained;
        CompletionEndpoint late;
        DispatchTicket old_ticket{JobId{old_run, 1}, 1, 1};
        ControlMessage delayed;
        {
            auto created = RunController::create(old_run, {}, Deadline::at(100ms), clock);
            REQUIRE(created);
            auto controller = std::move(created).value();
            Lookup lookup;
            lookup.controller = controller.get();
            ControllerCleanupObservationPort observation(lookup, clock);
            ResourceManager ledger(old_instance, host, observation);
            REQUIRE(ledger.observe_envelope({{1},
                                             old_instance,
                                             HostEpoch{1},
                                             1,
                                             100ms,
                                             capacity(),
                                             HostArbitration::enforced_generation,
                                             true,
                                             true},
                                            0ns));
            REQUIRE(controller->queue(controller->root()));
            ResourceRequest demand{OperationId{1},
                                   {controller->root(), AttemptId{1}, DispatchGeneration{1}},
                                   {1},
                                   HostEpoch{1},
                                   NativeWorkerGeneration{1},
                                   capacity(),
                                   capacity(),
                                   90ms,
                                   {}};
            auto reservation = ledger.reserve(demand, 0ns);
            REQUIRE(reservation);
            REQUIRE(ledger.acquired(
                {reservation.value(), old_instance, OperationId{2}, HostOutcome::acknowledged}));
            auto lease = ledger.commit_dispatch(reservation.value(), {true, true, true, true}, 0ns);
            REQUIRE(lease);
            ResourceVector allocation;
            allocation[ResourceKind::device] = 4;
            REQUIRE(ledger.materialized(
                reservation.value(),
                {old_instance, AllocationId{1}, HostEpoch{1}, NativeWorkerGeneration{1}},
                allocation));
            if (crash_stage > 0) {
                auto dispatched =
                    controller->dispatch(controller->root(), {true, true, true, true});
                REQUIRE(dispatched);
                old_ticket = dispatched.value();
            }
            auto mailbox = ControlMailbox::create(1, 1);
            REQUIRE(mailbox);
            LifecycleCorrelation correlation{
                old_ticket.job, AttemptId{old_ticket.attempt}, OperationId{1},
                DispatchGeneration{old_ticket.dispatch_generation}, SuspensionGeneration{0}};
            auto endpoint = mailbox.value()->reserve(correlation);
            REQUIRE(endpoint);
            late = endpoint.value();
            delayed = {ControlMessageKind::adapter_ready, correlation, OperationId{1},
                       std::nullopt};
            if (crash_stage == 2) {
                auto grant =
                    AdapterAuthority{}.authorize(context, policy, plan, registry, auth, old_ticket,
                                                 pin, JsonValue::Object{}, "old-operation");
                REQUIRE(grant);
                RegisteredCapabilityAdapter adapter(contract, provider);
                auto outcome =
                    adapter.invoke({std::move(grant).value(), JsonValue::Object{}, 128}, 1);
                REQUIRE(outcome.external_outcome == ExternalOutcome::unknown);
                REQUIRE(provider.accepted);
                // Crash before this outcome can reach the controller.
            }
            retained = controller->events();
            REQUIRE(host.releases.empty());
            REQUIRE(ledger.snapshot({1}).resident[ResourceKind::device] == 4);
        }
        REQUIRE(late.publish(delayed) == DeliveryStatus::closed);
        const auto writes = provider.calls, queries = provider.queries;
        const auto acquisitions = host.acquisitions.size(), releases = host.releases.size(),
                   reconciliations = host.reconciliations.size();
        REQUIRE(writes == (crash_stage == 2 ? 1U : 0U));
        auto created = RunController::create(new_run, {}, Deadline::at(100ms), clock);
        REQUIRE(created);
        auto new_controller = std::move(created).value();
        Lookup lookup;
        lookup.controller = new_controller.get();
        ControllerCleanupObservationPort observation(lookup, clock);
        ResourceManager new_ledger(new_instance, host, observation);
        ResourceRequest next{OperationId{1},
                             {new_controller->root(), AttemptId{1}, DispatchGeneration{1}},
                             {1},
                             HostEpoch{1},
                             NativeWorkerGeneration{1},
                             capacity(),
                             capacity(),
                             90ms,
                             {}};
        REQUIRE_FALSE(
            new_ledger.reserve(next, 0ns)); // No reconciled host envelope for the new incarnation.
        REQUIRE_FALSE(new_controller->complete(old_ticket));
        const auto watermark = new_controller->snapshot().watermark;
        const auto revision = new_ledger.snapshot({1}).revision;
        ReplayProjection replay;
        for (const auto &group : retained)
            REQUIRE(replay.apply({"flamoris.event/1", group, group.events.size(), false}));
        REQUIRE(replay.snapshot().playback);
        REQUIRE(new_controller->snapshot().watermark == watermark);
        REQUIRE(new_controller->snapshot().activity == RunActivity::created);
        REQUIRE(new_ledger.snapshot({1}).revision == revision);
        REQUIRE(provider.calls == writes);
        REQUIRE(provider.queries == queries);
        REQUIRE(host.acquisitions.size() == acquisitions);
        REQUIRE(host.releases.size() == releases);
        REQUIRE(host.reconciliations.size() == reconciliations);
        for (const auto &group : retained)
            for (const auto &event : group.events)
                if (event.to)
                    REQUIRE_FALSE(is_terminal(*event.to));
    }
}
