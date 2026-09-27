#include "../support/manual_activation_gateway.hpp"
#include "flamoris/runtime/runtime_activation.hpp"
#include <atomic>
#include <catch_amalgamated.hpp>
#include <mutex>
using namespace flamoris::runtime;
using namespace flamoris::runtime::test;
using namespace std::chrono_literals;
namespace {
class ActivationClock final : public MonotonicClock {
  public:
    std::atomic<std::int64_t> value{0};
    TimePoint now() const noexcept override { return TimePoint{value.load()}; }
};
class ActivationHost final : public HostAuthorityPort {
    std::mutex mutex_;
    std::vector<ResourceTicket> acquired_;
    std::vector<HostReleaseReceipt> released_;

  public:
    std::atomic<unsigned> acquire_count{0};
    Result<void> request_envelope(LogicalResourceId, RuntimeInstanceId) override {
        return Result<void>::success();
    }
    Result<void> acquire(const ResourceTicket &t, const ResourceVector &, TimePoint) override {
        std::lock_guard lock(mutex_);
        acquired_.push_back(t);
        ++acquire_count;
        return Result<void>::success();
    }
    Result<void> release(OperationId op, const ResourceTicket &t, OperationId grant) override {
        std::lock_guard lock(mutex_);
        released_.push_back({op, t, grant});
        return Result<void>::success();
    }
    Result<void> reconcile(OperationId, const ResourceTicket &, OperationId) override {
        return Result<void>::success();
    }
    void respond(RuntimeInstance &runtime) {
        std::vector<ResourceTicket> acquire;
        std::vector<HostReleaseReceipt> release;
        {
            std::lock_guard lock(mutex_);
            acquire.swap(acquired_);
            release.swap(released_);
        }
        for (const auto &t : acquire)
            REQUIRE(runtime.observe_host_acquisition(
                {t, runtime.instance_id(), t.operation, HostOutcome::acknowledged}));
        for (const auto &r : release)
            REQUIRE(runtime.observe_host_release(r));
    }
};
class ActivationEcho final : public RegisteredProviderPort {
  public:
    std::atomic<unsigned> calls{0};
    ExternalOutcome invoke(std::string_view, const JsonValue &, std::uint64_t,
                           BoundedProviderSink &sink) override {
        ++calls;
        return sink.append("\"ready\"") ? ExternalOutcome::confirmed_success
                                        : ExternalOutcome::confirmed_failure;
    }
    ExternalOutcome reconcile(std::string_view, std::uint64_t) override {
        return ExternalOutcome::unknown;
    }
};
AuthorizationContext caller() {
    AuthorizationContext value;
    value.subject = "activation-owner";
    value.expires_at_ms = 60000;
    value.capabilities = {"algorithm.echo"};
    value.permitted_effects = 63;
    value.access = {AccessSurface::status, AccessSurface::result, AccessSurface::events,
                    AccessSurface::cancel, AccessSurface::resume};
    return value;
}
std::string request() {
    return R"({"schema_version":"flamoris.submit/1","kind":"workflow","idempotency_key":"activation-test","workflow":{"schema_version":"flamoris.workflow/0.1","workflow":{"id":"test"},"inputs":{},"nodes":[{"id":"a","type":"algorithm.echo","with":{}}],"edges":[],"outputs":{"result":{"ref":{"source":"node","name":"a","path":[]}}},"limits":{}},"input_values":{}})";
}
struct Fixture {
    std::shared_ptr<ActivationClock> clock = std::make_shared<ActivationClock>();
    std::shared_ptr<ManualActivationPolicy> policy = std::make_shared<ManualActivationPolicy>();
    std::shared_ptr<ManualActivationGateway> gateway =
        std::make_shared<ManualActivationGateway>(*policy);
    std::shared_ptr<ActivationHost> host = std::make_shared<ActivationHost>();
    std::shared_ptr<ActivationEcho> echo = std::make_shared<ActivationEcho>();
    std::shared_ptr<RuntimeInstance> runtime;
    std::unique_ptr<RuntimeActivationClient> client;
    Fixture() {
        RuntimeConfiguration config;
        config.instance = {909, 1};
        config.clock = clock;
        config.host = host;
        CapabilityContract capability(EffectSet::from_mask(1).value());
        capability.identifier = "algorithm.echo";
        capability.version = "1";
        capability.adapter_revision = "echo/1";
        capability.input_schema.kind = ValueSchema::Kind::object;
        capability.output_schema.kind = ValueSchema::Kind::string;
        capability.output_schema.max_bytes = 16;
        capability.cancellable = true;
        config.capabilities.capabilities.emplace(capability.identifier, capability);
        config.policy.capabilities = caller().capabilities;
        config.policy.permitted_effects = 63;
        config.policy.access = caller().access;
        RuntimeRegistration registration;
        registration.capability = capability.identifier;
        registration.provider = echo;
        registration.requirements[ResourceKind::execution] = 1;
        registration.requirements[ResourceKind::adapter] = 1;
        registration.run_resource_limit.values.fill(10);
        config.registrations.push_back(registration);
        auto created = RuntimeInstance::create(std::move(config));
        REQUIRE(created);
        runtime = std::shared_ptr<RuntimeInstance>(std::move(created).value());
        auto outer = RuntimeActivationClient::create(
            {{"runtime.native", 1, 1}, HostEpoch{1}, clock, gateway, policy, 8});
        REQUIRE(outer);
        client = std::move(outer).value();
    }
    void deliver() {
        auto ready = std::move(gateway->observations);
        gateway->observations.clear();
        gateway->observations.reserve(128);
        for (auto observation : ready)
            REQUIRE(client->observe(std::move(observation)));
    }
    void activate() {
        REQUIRE(client->trigger(OperationId{1}, 1s));
        deliver();
        REQUIRE(gateway->perform_start(0, 0ns));
        REQUIRE(gateway->ready(0, runtime->instance_id()));
        deliver();
        REQUIRE(client->attach_endpoint(OperationId{1}, runtime));
    }
    void envelope() {
        HostEnvelope envelope;
        envelope.resource = {1};
        envelope.instance = runtime->instance_id();
        envelope.epoch = HostEpoch{1};
        envelope.authority_revision = 1;
        envelope.expires = 60s;
        envelope.capacity.values.fill(10);
        envelope.arbitration = HostArbitration::enforced_generation;
        envelope.contract_verified = envelope.reconciled_inventory = true;
        REQUIRE(runtime->observe_host_envelope(envelope));
    }
    RunResult finish(RunId run) {
        for (unsigned i = 0; i < 2000; ++i) {
            host->respond(*runtime);
            REQUIRE(runtime->poll());
            auto result = runtime->result(caller(), run);
            REQUIRE(result);
            if (!result.value().pending)
                return result.value();
        }
        FAIL("bounded activated Runtime progress did not finish");
        return {};
    }
};
} // namespace
TEST_CASE(
    "B-ACT01 B-ACT02 B-ACT10 configured activation routes only READY to real Runtime authority",
    "[runtime][activation]") {
    Fixture f;
    REQUIRE(f.client->trigger(OperationId{1}, 1s));
    REQUIRE(f.client->trigger(OperationId{2}, 1s));
    f.deliver();
    REQUIRE_FALSE(f.client->attach_endpoint(OperationId{1}, f.runtime));
    REQUIRE_FALSE(f.client->submit(OperationId{1}, caller(), request()));
    REQUIRE(f.gateway->perform_start(0, 0ns));
    REQUIRE(f.gateway->ready(0, f.runtime->instance_id()));
    f.deliver();
    REQUIRE(f.client->attach_endpoint(OperationId{1}, f.runtime));
    REQUIRE(f.client->attach_endpoint(OperationId{2}, f.runtime));
    auto admitted = f.client->submit(OperationId{1}, caller(), request());
    REQUIRE(admitted);
    for (unsigned i = 0; i < 4; ++i)
        REQUIRE(f.runtime->poll());
    REQUIRE(f.echo->calls == 0);
    REQUIRE(f.host->acquire_count == 0);
    REQUIRE(f.client->detach(OperationId{1}));
    REQUIRE(f.gateway->host_stops == 0);
    auto duplicate = f.client->submit(OperationId{2}, caller(), request());
    REQUIRE(duplicate);
    REQUIRE(duplicate.value().id == admitted.value().id);
    f.envelope();
    auto completed = f.finish(admitted.value().id);
    REQUIRE_FALSE(completed.error);
    REQUIRE(f.echo->calls == 1);
    REQUIRE(f.gateway->host_starts == 1);
}
TEST_CASE("B-ACT05 B-ACT08 late gateway receipts cannot route a fenced or destroyed Runtime",
          "[runtime][activation]") {
    Fixture f;
    f.activate();
    f.envelope();
    auto original = f.client->status(OperationId{1});
    REQUIRE(original);
    REQUIRE(f.client->host_epoch_lost(HostEpoch{1}));
    REQUIRE_FALSE(f.client->submit(OperationId{1}, caller(), request()));
    REQUIRE_FALSE(f.runtime->submit(caller(), request()));
    REQUIRE_FALSE(f.client->observe(original.value()));
    auto snapshot = f.runtime->resource_snapshot({1});
    REQUIRE(snapshot);
    REQUIRE(snapshot.value().resident.empty());
    f.runtime.reset();
    REQUIRE_FALSE(f.client->submit(OperationId{1}, caller(), request()));
}
TEST_CASE("B-ACT06 B-ACT09 authorized idle stop closes real admission before host drain",
          "[runtime][activation]") {
    Fixture f;
    f.activate();
    f.policy->allowed[static_cast<std::size_t>(ActivationScope::stop)] = false;
    REQUIRE_FALSE(f.client->request_idle_stop(OperationId{1}, 1s));
    REQUIRE(f.runtime->admission_epoch().value() == 1);
    REQUIRE(f.gateway->host_stops == 0);
    f.policy->allowed[static_cast<std::size_t>(ActivationScope::stop)] = true;
    SECTION("idle closure wins and the old endpoint never silently reopens") {
        REQUIRE(f.client->request_idle_stop(OperationId{1}, 1s));
        REQUIRE(f.gateway->host_stops == 1);
        REQUIRE(f.runtime->admission_epoch().value() == 2);
        REQUIRE_FALSE(f.client->submit(OperationId{1}, caller(), request()));
        REQUIRE_FALSE(f.runtime->submit(caller(), request()));
    }
    SECTION("an admitted pending Run prevents idle closure") {
        auto admitted = f.client->submit(OperationId{1}, caller(), request());
        REQUIRE(admitted);
        REQUIRE_FALSE(f.client->request_idle_stop(OperationId{1}, 1s));
        REQUIRE(f.gateway->host_stops == 0);
        REQUIRE(f.runtime->admission_epoch().value() == 1);
        f.envelope();
        REQUIRE_FALSE(f.finish(admitted.value().id).error);
        REQUIRE(f.client->request_idle_stop(OperationId{1}, 1s));
        REQUIRE(f.gateway->host_stops == 1);
    }
}
TEST_CASE("B-CALL01 an attached activation endpoint may die before a late host callback",
          "[runtime][activation][lifetime]") {
    Fixture f;
    f.activate();
    auto ready = f.client->status(OperationId{1});
    REQUIRE(ready);
    std::weak_ptr<RuntimeInstance> lifetime = f.runtime;
    f.runtime.reset();
    REQUIRE(lifetime.expired());
    REQUIRE_FALSE(f.client->submit(OperationId{1}, caller(), request()));
    auto late = ready.value();
    late.status = ProcessStatus::unknown;
    REQUIRE(f.client->observe(late));
    REQUIRE(f.client->detach(OperationId{1}));
    REQUIRE(f.client->retire_detached() == 1);
    REQUIRE(f.gateway->host_stops == 0);
}
