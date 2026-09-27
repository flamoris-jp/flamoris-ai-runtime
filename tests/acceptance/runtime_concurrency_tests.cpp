#include "catch_amalgamated.hpp"
#include "flamoris/runtime/runtime.hpp"
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <sstream>

using namespace flamoris::runtime;
using namespace std::chrono_literals;
namespace {
class Clock final : public MonotonicClock {
  public:
    std::atomic<std::int64_t> ticks{};
    TimePoint now() const noexcept override { return TimePoint{ticks.load()}; }
};
struct Claims {
    std::mutex mutex;
    std::condition_variable changed;
    unsigned owners{0}, waiters{0};
    bool released{false}, reject_owner{false};
    void arrive(bool owner) {
        std::unique_lock lock(mutex);
        owner ? ++owners : ++waiters;
        changed.notify_all();
        if (owner) {
            changed.wait(lock, [&] { return released; });
            if (reject_owner)
                throw std::runtime_error("private admission preparation detail");
        }
    }
    bool await(unsigned wanted_owners, unsigned wanted_waiters) {
        std::unique_lock lock(mutex);
        return changed.wait_for(
            lock, 5s, [&] { return owners >= wanted_owners && waiters >= wanted_waiters; });
    }
    void release(bool reject = false) {
        std::lock_guard lock(mutex);
        reject_owner = reject;
        released = true;
        changed.notify_all();
    }
};
class Host final : public HostAuthorityPort {
  public:
    std::mutex mutex;
    std::vector<ResourceTicket> acquisitions;
    std::vector<RuntimeHostRelease> releases;
    std::atomic<unsigned> acquire_calls{0}, release_calls{0}, reconcile_calls{0};
    Result<void> request_envelope(LogicalResourceId, RuntimeInstanceId) override {
        return Result<void>::success();
    }
    Result<void> acquire(const ResourceTicket &ticket, const ResourceVector &, TimePoint) override {
        std::lock_guard lock(mutex);
        ++acquire_calls;
        acquisitions.push_back(ticket);
        return Result<void>::success();
    }
    Result<void> release(OperationId operation, const ResourceTicket &ticket,
                         OperationId grant) override {
        std::lock_guard lock(mutex);
        ++release_calls;
        releases.push_back({operation, ticket, grant});
        return Result<void>::success();
    }
    Result<void> reconcile(OperationId, const ResourceTicket &, OperationId) override {
        ++reconcile_calls;
        return Result<void>::success();
    }
    void respond(RuntimeInstance &runtime) {
        std::vector<ResourceTicket> pending_acquisitions;
        std::vector<RuntimeHostRelease> pending_releases;
        {
            std::lock_guard lock(mutex);
            pending_acquisitions.swap(acquisitions);
            pending_releases.swap(releases);
        }
        for (const auto &ticket : pending_acquisitions)
            REQUIRE(runtime.observe_host_acquisition(
                {ticket, runtime.instance_id(), ticket.operation, HostOutcome::acknowledged}));
        for (const auto &release : pending_releases)
            REQUIRE(runtime.observe_host_release(release));
    }
};
class Provider final : public RegisteredProviderPort {
  public:
    std::atomic<unsigned> calls{0}, queries{0};
    std::mutex mutex;
    std::condition_variable changed;
    bool block{false}, entered{false}, released{false};
    ExternalOutcome invoke(std::string_view, const JsonValue &input, std::uint64_t,
                           BoundedProviderSink &sink) override {
        ++calls;
        {
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            if (block)
                changed.wait(lock, [&] { return released; });
        }
        sink.record_outcome(ExternalOutcome::confirmed_success);
        auto encoded = canonical_json(std::get<JsonValue::Object>(input.data).at("text"));
        if (encoded)
            (void)sink.append(encoded.value());
        return ExternalOutcome::confirmed_success;
    }
    ExternalOutcome reconcile(std::string_view, std::uint64_t) override {
        ++queries;
        return ExternalOutcome::unknown;
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        changed.notify_all();
    }
};
AuthorizationContext caller() {
    AuthorizationContext context;
    context.subject = "owner";
    context.expires_at_ms = 60000;
    context.capabilities = {"algorithm.echo"};
    context.permitted_effects = 63;
    context.access = {AccessSurface::status, AccessSurface::result, AccessSurface::events,
                      AccessSurface::replay, AccessSurface::cancel, AccessSurface::resume};
    return context;
}
CapabilityContract capability() {
    CapabilityContract contract(EffectSet::from_names({"pure"}).value());
    contract.identifier = "algorithm.echo";
    contract.version = "1";
    contract.adapter_revision = "registered/1";
    ValueSchema text;
    text.kind = ValueSchema::Kind::string;
    text.max_bytes = 128;
    contract.input_schema.kind = ValueSchema::Kind::object;
    contract.input_schema.properties.emplace("text", text);
    contract.input_schema.required.insert("text");
    contract.output_schema = text;
    return contract;
}
std::string workflow(bool keyed = true, std::string_view text = "hello") {
    return "{\"schema_version\":\"flamoris.submit/1\",\"kind\":\"workflow\"," +
           std::string(keyed ? "\"idempotency_key\":\"same\"," : "") +
           "\"workflow\":{\"schema_version\":\"flamoris.workflow/"
           "0.1\",\"workflow\":{\"id\":\"test\"},\"inputs\":{},"
           "\"nodes\":[{\"id\":\"a\",\"type\":\"algorithm.echo\",\"with\":{\"text\":{\"literal\":"
           "\"" +
           std::string(text) +
           "\"}}}],\"edges\":[],\"outputs\":{\"result\":{\"ref\":{\"source\":\"node\",\"name\":"
           "\"a\",\"path\":[]}}},\"limits\":{}},\"input_values\":{}}";
}
struct Fixture {
    std::shared_ptr<Clock> clock = std::make_shared<Clock>();
    std::shared_ptr<Host> host = std::make_shared<Host>();
    std::shared_ptr<Provider> provider = std::make_shared<Provider>();
    CapabilitySnapshot registry;
    std::unique_ptr<RuntimeInstance> runtime;
    explicit Fixture(std::shared_ptr<Claims> claims = {},
                     std::function<void(RuntimeConfiguration &)> configure = {}) {
        RuntimeConfiguration config;
        config.instance = {121, 1};
        config.clock = clock;
        config.host = host;
        auto contract = capability();
        registry.capabilities.emplace(contract.identifier, contract);
        config.capabilities = registry;
        config.policy.capabilities = caller().capabilities;
        config.policy.permitted_effects = 63;
        config.policy.access = caller().access;
        if (claims)
            config.submission_preparation_hook = [claims](PendingSubmissionId, bool owner) {
                claims->arrive(owner);
            };
        RuntimeRegistration registration;
        registration.capability = contract.identifier;
        registration.provider = provider;
        registration.requirements[ResourceKind::adapter] = 1;
        registration.requirements[ResourceKind::execution] = 1;
        registration.run_resource_limit[ResourceKind::adapter] = 8;
        registration.run_resource_limit[ResourceKind::execution] = 8;
        config.registrations.push_back(registration);
        if (configure)
            configure(config);
        registry = config.capabilities;
        auto created = RuntimeInstance::create(std::move(config));
        REQUIRE(created);
        runtime = std::move(created).value();
    }
    void envelope() {
        HostEnvelope envelope;
        envelope.instance = runtime->instance_id();
        envelope.resource = {1};
        envelope.epoch = HostEpoch{1};
        envelope.authority_revision = 1;
        envelope.expires = 60s;
        envelope.capacity.values.fill(1048576);
        envelope.arbitration = HostArbitration::enforced_generation;
        envelope.contract_verified = true;
        envelope.reconciled_inventory = true;
        REQUIRE(runtime->observe_host_envelope(envelope));
    }
    RunResult finish(RunId id, AuthorizationContext context = caller()) {
        for (unsigned turn = 0; turn < 10000; ++turn) {
            host->respond(*runtime);
            REQUIRE(runtime->poll());
            auto result = runtime->result(context, id);
            REQUIRE(result);
            if (!result.value().pending)
                return result.value();
        }
        FAIL("bounded production actor did not finish");
        return {};
    }
};
} // namespace

TEST_CASE(
    "A27 production Runtime pending duplicate shares one admission and one provider handoff") {
    auto claims = std::make_shared<Claims>();
    Fixture fixture(claims);
    auto owner = std::async(std::launch::async,
                            [&] { return fixture.runtime->submit(caller(), workflow()); });
    bool owner_ready = claims->await(1, 0);
    if (!owner_ready)
        claims->release();
    REQUIRE(owner_ready);
    auto waiter = std::async(std::launch::async,
                             [&] { return fixture.runtime->submit(caller(), workflow()); });
    bool waiter_ready = claims->await(1, 1);
    auto conflict = fixture.runtime->submit(caller(), workflow(true, "changed"));
    claims->release();
    REQUIRE(waiter_ready);
    REQUIRE_FALSE(conflict);
    auto admitted = owner.get();
    auto duplicate = waiter.get();
    REQUIRE(admitted);
    REQUIRE(duplicate);
    REQUIRE(admitted.value().id == duplicate.value().id);
    REQUIRE(duplicate.value().duplicate);
    fixture.envelope();
    auto result = fixture.finish(admitted.value().id);
    REQUIRE_FALSE(result.error);
    REQUIRE(fixture.provider->calls == 1);
    auto changed = fixture.registry;
    changed.capabilities.at("algorithm.echo").adapter_revision = "2";
    REQUIRE(fixture.runtime->replace_capabilities(changed));
    auto retained = fixture.runtime->submit(caller(), workflow());
    REQUIRE(retained);
    REQUIRE(retained.value().id == admitted.value().id);
    REQUIRE(fixture.provider->calls == 1);
}

TEST_CASE("A27 production Runtime broadcasts owner rejection then releases keyed claim") {
    auto claims = std::make_shared<Claims>();
    Fixture fixture(claims);
    auto owner = std::async(std::launch::async,
                            [&] { return fixture.runtime->submit(caller(), workflow()); });
    bool owner_ready = claims->await(1, 0);
    if (!owner_ready)
        claims->release();
    REQUIRE(owner_ready);
    auto waiter = std::async(std::launch::async,
                             [&] { return fixture.runtime->submit(caller(), workflow()); });
    bool waiter_ready = claims->await(1, 1);
    claims->release(true);
    REQUIRE(waiter_ready);
    auto rejected = owner.get();
    auto duplicate = waiter.get();
    REQUIRE_FALSE(rejected);
    REQUIRE_FALSE(duplicate);
    REQUIRE(rejected.error() == duplicate.error());
    REQUIRE(fixture.provider->calls == 0);
    claims->release(false);
    auto fresh = fixture.runtime->submit(caller(), workflow());
    REQUIRE(fresh);
    fixture.envelope();
    REQUIRE_FALSE(fixture.finish(fresh.value().id).error);
    REQUIRE(fixture.provider->calls == 1);
}

TEST_CASE("A27 production Runtime unkeyed concurrent requests retain independent owners") {
    auto claims = std::make_shared<Claims>();
    Fixture fixture(claims);
    auto first = std::async(std::launch::async,
                            [&] { return fixture.runtime->submit(caller(), workflow(false)); });
    auto second = std::async(std::launch::async,
                             [&] { return fixture.runtime->submit(caller(), workflow(false)); });
    bool both = claims->await(2, 0);
    claims->release();
    REQUIRE(both);
    auto one = first.get(), two = second.get();
    REQUIRE(one);
    REQUIRE(two);
    REQUIRE(one.value().id != two.value().id);
    REQUIRE_FALSE(one.value().duplicate);
    REQUIRE_FALSE(two.value().duplicate);
    fixture.envelope();
    REQUIRE_FALSE(fixture.finish(one.value().id).error);
    REQUIRE_FALSE(fixture.finish(two.value().id).error);
    REQUIRE(fixture.provider->calls == 2);
}

TEST_CASE(
    "A32 production Runtime replay preserves live ledger lifecycle and external port counts") {
    Fixture fixture;
    auto admitted = fixture.runtime->submit(caller(), workflow());
    REQUIRE(admitted);
    fixture.envelope();
    REQUIRE_FALSE(fixture.finish(admitted.value().id).error);
    auto before = fixture.runtime->status(caller(), admitted.value().id);
    REQUIRE(before);
    auto ledger = fixture.runtime->resource_snapshot({1});
    REQUIRE(ledger);
    auto calls = fixture.provider->calls.load(), queries = fixture.provider->queries.load();
    auto acquire = fixture.host->acquire_calls.load(), release = fixture.host->release_calls.load(),
         reconcile = fixture.host->reconcile_calls.load();
    auto replay = fixture.runtime->replay(caller(), admitted.value().id);
    REQUIRE(replay);
    REQUIRE(replay.value().complete);
    auto after = fixture.runtime->status(caller(), admitted.value().id);
    REQUIRE(after);
    auto after_ledger = fixture.runtime->resource_snapshot({1});
    REQUIRE(after_ledger);
    REQUIRE(before.value().watermark == after.value().watermark);
    REQUIRE(before.value().activity == after.value().activity);
    REQUIRE(ledger.value().revision == after_ledger.value().revision);
    REQUIRE(ledger.value().resident == after_ledger.value().resident);
    REQUIRE(fixture.provider->calls == calls);
    REQUIRE(fixture.provider->queries == queries);
    REQUIRE(fixture.host->acquire_calls == acquire);
    REQUIRE(fixture.host->release_calls == release);
    REQUIRE(fixture.host->reconcile_calls == reconcile);
    CallerFacade facade(*fixture.runtime);
    std::istringstream disconnected;
    std::ostringstream output;
    REQUIRE(serve_json_lines(disconnected, output, facade, caller()) == 0);
    REQUIRE(fixture.runtime->status(caller(), admitted.value().id).value().activity ==
            RunActivity::succeeded);
}

TEST_CASE(
    "B-CALL01 B-DRAIN01 production shutdown preserves cancellation before late provider success") {
    Fixture fixture;
    fixture.provider->block = true;
    auto admitted = fixture.runtime->submit(caller(), workflow());
    REQUIRE(admitted);
    fixture.envelope();
    for (unsigned turn = 0; turn < 10000 && fixture.provider->calls == 0; ++turn) {
        fixture.host->respond(*fixture.runtime);
        REQUIRE(fixture.runtime->poll());
    }
    const bool entered = fixture.provider->calls == 1;
    auto stopped = fixture.runtime->shutdown();
    fixture.provider->release();
    REQUIRE(entered);
    REQUIRE(stopped);
    auto terminal = fixture.finish(admitted.value().id);
    REQUIRE(terminal.error);
    REQUIRE(fixture.runtime->status(caller(), admitted.value().id).value().activity ==
            RunActivity::cancelled);
    REQUIRE(fixture.provider->calls == 1);
    REQUIRE_FALSE(fixture.runtime->submit(caller(), workflow(false)));
}

TEST_CASE("C12 terminal retention returns bounded Run capacity to admission") {
    Fixture fixture({}, [](RuntimeConfiguration &config) {
        config.max_runs = 1;
        config.submissions.retention_ms = 10;
    });
    fixture.envelope();
    auto first = fixture.runtime->submit(caller(), workflow(false));
    REQUIRE(first);
    REQUIRE_FALSE(fixture.finish(first.value().id).error);
    REQUIRE_FALSE(fixture.runtime->submit(caller(), workflow(false)));
    fixture.clock->ticks = 11000000;
    REQUIRE(fixture.runtime->poll());
    REQUIRE_FALSE(fixture.runtime->status(caller(), first.value().id));
    auto second = fixture.runtime->submit(caller(), workflow(false));
    REQUIRE(second);
    REQUIRE(second.value().id != first.value().id);
    REQUIRE_FALSE(fixture.finish(second.value().id).error);
    REQUIRE(fixture.provider->calls == 2);
}

TEST_CASE("C12 adapter retirement floor preserves an older pending admission") {
    auto first_claim = std::make_shared<Claims>();
    Fixture fixture({}, [first_claim](RuntimeConfiguration &config) {
        config.max_runs = 1;
        config.submissions.retention_ms = 10;
        config.submission_preparation_hook = [first_claim](PendingSubmissionId id, bool owner) {
            if (id.value() == 1 && owner)
                first_claim->arrive(true);
        };
    });
    fixture.envelope();
    auto pending = std::async(std::launch::async,
                              [&] { return fixture.runtime->submit(caller(), workflow(false)); });
    struct ReleaseClaim {
        std::shared_ptr<Claims> claim;
        ~ReleaseClaim() { claim->release(); }
    } release_claim{first_claim};
    const bool claimed = first_claim->await(1, 0);
    if (!claimed)
        first_claim->release();
    REQUIRE(claimed);
    auto newer = fixture.runtime->submit(caller(), workflow(false));
    if (!newer)
        first_claim->release();
    REQUIRE(newer);
    auto completed = fixture.finish(newer.value().id);
    fixture.clock->ticks = 11000000;
    auto progressed = fixture.runtime->poll();
    first_claim->release();
    auto older = pending.get();
    REQUIRE_FALSE(completed.error);
    REQUIRE(progressed);
    REQUIRE(older);
    REQUIRE(older.value().id.value() < newer.value().id.value());
    REQUIRE_FALSE(fixture.finish(older.value().id).error);
    REQUIRE(fixture.provider->calls == 2);
}

TEST_CASE("A35 production stored result rechecks current handle access and producer identity") {
    Fixture fixture({}, [](RuntimeConfiguration &config) {
        auto &contract = config.capabilities.capabilities.at("algorithm.echo");
        contract.output_schema.service_handle_type = "registered.asset/1";
        contract.handle_validator_revision = "asset-validator/1";
        contract.handle_validator = [](const JsonValue &value, std::string_view, std::uint64_t) {
            if (std::get<std::string>(value.data) != "opaque-handle")
                return Result<ValidatedHandleAccess>::failure(
                    ErrorEnvelope::make(ErrorCode::permission_denied));
            return Result<ValidatedHandleAccess>::success({"owner", "object.a", 50});
        };
        config.policy.access.insert(AccessSurface::handle);
        config.policy.object_scopes.insert("object.a");
    });
    auto context = caller();
    context.access.insert(AccessSurface::handle);
    context.object_scopes.insert("object.a");
    fixture.envelope();
    auto admitted = fixture.runtime->submit(context, workflow(false, "opaque-handle"));
    REQUIRE(admitted);
    REQUIRE_FALSE(fixture.finish(admitted.value().id, context).error);
    auto without_handle = context;
    without_handle.access.erase(AccessSurface::handle);
    REQUIRE_FALSE(fixture.runtime->result(without_handle, admitted.value().id));
    auto without_scope = context;
    without_scope.object_scopes.clear();
    REQUIRE_FALSE(fixture.runtime->result(without_scope, admitted.value().id));
    auto revoked = context;
    revoked.revoked = true;
    REQUIRE_FALSE(fixture.runtime->result(revoked, admitted.value().id));
    REQUIRE(fixture.runtime->result(context, admitted.value().id));
    PolicySnapshot policy;
    policy.revision = 2;
    policy.capabilities = context.capabilities;
    policy.permitted_effects = context.permitted_effects;
    policy.access = context.access;
    REQUIRE(fixture.runtime->replace_policy(policy));
    REQUIRE_FALSE(fixture.runtime->result(context, admitted.value().id));
    policy.revision = 3;
    policy.object_scopes = context.object_scopes;
    REQUIRE(fixture.runtime->replace_policy(policy));
    REQUIRE(fixture.runtime->result(context, admitted.value().id));
    auto changed = fixture.registry;
    changed.capabilities.at("algorithm.echo").handle_validator_revision = "asset-validator/2";
    REQUIRE(fixture.runtime->replace_capabilities(changed));
    REQUIRE_FALSE(fixture.runtime->result(context, admitted.value().id));
    REQUIRE(fixture.runtime->replace_capabilities(fixture.registry));
    REQUIRE(fixture.runtime->result(context, admitted.value().id));
    fixture.clock->ticks = 50000000;
    REQUIRE_FALSE(fixture.runtime->result(context, admitted.value().id));
    REQUIRE(fixture.provider->calls == 1);
}
