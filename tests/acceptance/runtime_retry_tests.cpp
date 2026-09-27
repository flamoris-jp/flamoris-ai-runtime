#include "catch_amalgamated.hpp"
#include "flamoris/runtime/runtime.hpp"
#include <algorithm>
#include <atomic>
#include <mutex>

using namespace flamoris::runtime;
using namespace std::chrono_literals;
namespace {
class RetryClock final : public MonotonicClock {
  public:
    std::atomic<std::int64_t> ticks{};
    TimePoint now() const noexcept override { return TimePoint{ticks.load()}; }
    void at(TimePoint time) { ticks.store(time.count()); }
};
class RetryHost final : public HostAuthorityPort {
  public:
    std::mutex mutex;
    std::vector<ResourceTicket> acquisitions;
    std::vector<RuntimeHostRelease> releases;
    Result<void> request_envelope(LogicalResourceId, RuntimeInstanceId) override {
        return Result<void>::success();
    }
    Result<void> acquire(const ResourceTicket &ticket, const ResourceVector &, TimePoint) override {
        std::lock_guard lock(mutex);
        acquisitions.push_back(ticket);
        return Result<void>::success();
    }
    Result<void> release(OperationId operation, const ResourceTicket &ticket,
                         OperationId grant) override {
        std::lock_guard lock(mutex);
        releases.push_back({operation, ticket, grant});
        return Result<void>::success();
    }
    Result<void> reconcile(OperationId, const ResourceTicket &, OperationId) override {
        return Result<void>::success();
    }
    void deliver(RuntimeInstance &runtime) {
        std::vector<ResourceTicket> starts;
        std::vector<RuntimeHostRelease> stops;
        {
            std::lock_guard lock(mutex);
            starts.swap(acquisitions);
            stops.swap(releases);
        }
        for (const auto &start : starts)
            REQUIRE(runtime.observe_host_acquisition(
                {start, runtime.instance_id(), start.operation, HostOutcome::acknowledged}));
        for (const auto &stop : stops)
            REQUIRE(runtime.observe_host_release(stop));
    }
};
class RetryProvider final : public RegisteredProviderPort {
  public:
    std::atomic<unsigned> calls{};
    bool unknown{};
    std::mutex mutex;
    std::vector<std::string> operation_keys;
    std::vector<std::uint64_t> deadlines;
    ExternalOutcome invoke(std::string_view key, const JsonValue &, std::uint64_t deadline,
                           BoundedProviderSink &sink) override {
        const auto index = calls.fetch_add(1);
        {
            std::lock_guard lock(mutex);
            operation_keys.emplace_back(key);
            deadlines.push_back(deadline);
        }
        if (index == 0 || unknown) {
            // An explicitly configured provider supplies proof; the Runtime must not
            // infer it merely because an error happens to look transient.
            sink.record_retry_evidence({true, true, true});
            return unknown ? ExternalOutcome::unknown : ExternalOutcome::confirmed_failure;
        }
        if (!sink.append("\"second attempt\""))
            return ExternalOutcome::confirmed_failure;
        return ExternalOutcome::confirmed_success;
    }
    ExternalOutcome reconcile(std::string_view, std::uint64_t) override {
        return ExternalOutcome::unknown;
    }
};
AuthorizationContext retry_caller() {
    AuthorizationContext caller;
    caller.subject = "retry-owner";
    caller.expires_at_ms = 1000;
    caller.capabilities = {"algorithm.retry"};
    caller.permitted_effects = 63;
    caller.access = {AccessSurface::status, AccessSurface::result, AccessSurface::events,
                     AccessSurface::cancel};
    return caller;
}
struct RetryFixture {
    std::shared_ptr<RetryClock> clock = std::make_shared<RetryClock>();
    std::shared_ptr<RetryHost> host = std::make_shared<RetryHost>();
    std::shared_ptr<RetryProvider> provider = std::make_shared<RetryProvider>();
    std::unique_ptr<RuntimeInstance> runtime;
    PolicySnapshot policy;
    explicit RetryFixture(bool unknown = false) {
        provider->unknown = unknown;
        RuntimeConfiguration config;
        config.instance = {0x51, 0x82};
        config.clock = clock;
        config.host = host;
        CapabilityContract contract(EffectSet::from_mask(unknown ? 12 : 1).value());
        contract.identifier = "algorithm.retry";
        contract.version = "1";
        contract.adapter_revision = "retry-proof/1";
        contract.input_schema.kind = ValueSchema::Kind::object;
        contract.output_schema.kind = ValueSchema::Kind::string;
        contract.output_schema.max_bytes = 64;
        contract.retry_permitted = true;
        contract.provider_deduplication = !unknown;
        contract.max_attempts = 2;
        contract.max_timeout_ms = 100;
        contract.max_output_bytes = 128;
        config.capabilities.capabilities.emplace(contract.identifier, contract);
        policy.capabilities = retry_caller().capabilities;
        policy.permitted_effects = 63;
        policy.access = retry_caller().access;
        config.policy = policy;
        RuntimeRegistration registration;
        registration.capability = contract.identifier;
        registration.provider = provider;
        registration.requirements[ResourceKind::execution] = 1;
        registration.requirements[ResourceKind::adapter] = 1;
        registration.run_resource_limit[ResourceKind::execution] = 2;
        registration.run_resource_limit[ResourceKind::adapter] = 2;
        config.registrations.push_back(std::move(registration));
        auto created = RuntimeInstance::create(std::move(config));
        REQUIRE(created);
        runtime = std::move(created).value();
        HostEnvelope envelope;
        envelope.instance = runtime->instance_id();
        envelope.resource = {1};
        envelope.epoch = HostEpoch{1};
        envelope.authority_revision = 1;
        envelope.expires = 1s;
        envelope.capacity.values.fill(128);
        envelope.arbitration = HostArbitration::enforced_generation;
        envelope.contract_verified = true;
        envelope.reconciled_inventory = true;
        REQUIRE(runtime->observe_host_envelope(envelope));
    }
    RunId submit() {
        const auto input =
            R"({"schema_version":"flamoris.submit/1","kind":"workflow","workflow":{"schema_version":"flamoris.workflow/0.1","workflow":{"id":"retry"},"inputs":{},"nodes":[{"id":"operation","type":"algorithm.retry","with":{},"retry":{"max_attempts":2,"backoff_ms":25}}],"edges":[],"outputs":{"result":{"ref":{"source":"node","name":"operation","path":[]}}},"limits":{"timeout_ms":100}},"input_values":{}})";
        auto submitted = runtime->submit(retry_caller(), input);
        REQUIRE(submitted);
        return submitted.value().id;
    }
    RunSnapshot tick(RunId run) {
        host->deliver(*runtime);
        REQUIRE(runtime->poll());
        auto snapshot = runtime->status(retry_caller(), run);
        REQUIRE(snapshot);
        return snapshot.value();
    }
    JobSnapshot await_backoff(RunId run) {
        for (unsigned i = 0; i < 10000; ++i) {
            auto snapshot = tick(run);
            for (const auto &job : snapshot.jobs)
                if (job.id != snapshot.root && job.state == JobState::queued && job.attempt == 2)
                    return job;
            auto result = runtime->result(retry_caller(), run);
            REQUIRE(result);
            REQUIRE(result.value().pending);
        }
        FAIL("eligible explicit retry never reached backoff");
        return JobSnapshot{{}, {}, JobState::created, {}, {}, Deadline::at(0ns)};
    }
    RunResult finish(RunId run) {
        for (unsigned i = 0; i < 10000; ++i) {
            (void)tick(run);
            auto result = runtime->result(retry_caller(), run);
            REQUIRE(result);
            if (!result.value().pending)
                return result.value();
        }
        FAIL("retry integration did not settle bounded ownership");
        return {};
    }
};
} // namespace

TEST_CASE("B-RETRY01 Runtime retries confirmed transient no-effect with stable Job key and "
          "original deadline") {
    RetryFixture fixture;
    const auto run = fixture.submit();
    auto waiting = fixture.await_backoff(run);
    REQUIRE(fixture.provider->calls == 1);
    REQUIRE(waiting.deadline.time() == 100ms);
    fixture.clock->at(24ms);
    for (unsigned i = 0; i < 16; ++i)
        (void)fixture.tick(run);
    REQUIRE(fixture.provider->calls == 1);
    fixture.clock->at(25ms);
    auto result = fixture.finish(run);
    INFO("provider calls " << fixture.provider->calls.load());
    INFO("result error " << (result.error ? to_string(result.error->code()) : "none"));
    REQUIRE_FALSE(result.error);
    REQUIRE(result.value == JsonValue{JsonValue::Object{{"result", "second attempt"}}});
    REQUIRE(fixture.provider->calls == 2);
    auto snapshot = fixture.runtime->status(retry_caller(), run);
    REQUIRE(snapshot);
    REQUIRE(snapshot.value().jobs.size() == 2);
    auto child = std::find_if(snapshot.value().jobs.begin(), snapshot.value().jobs.end(),
                              [&](const auto &value) { return value.id == waiting.id; });
    REQUIRE(child != snapshot.value().jobs.end());
    REQUIRE(child->attempt == 2);
    REQUIRE(child->deadline.time() == 100ms);
    std::lock_guard lock(fixture.provider->mutex);
    REQUIRE(fixture.provider->operation_keys.size() == 2);
    REQUIRE(fixture.provider->operation_keys[0] == fixture.provider->operation_keys[1]);
    REQUIRE(fixture.provider->deadlines == std::vector<std::uint64_t>{100, 100});
}

TEST_CASE(
    "B-RETRY01 Runtime current revocation denies a queued retry without another provider call") {
    RetryFixture fixture;
    const auto run = fixture.submit();
    auto waiting = fixture.await_backoff(run);
    ++fixture.policy.revision;
    fixture.policy.capabilities.clear();
    REQUIRE(fixture.runtime->replace_policy(fixture.policy));
    fixture.clock->at(25ms);
    auto result = fixture.finish(run);
    REQUIRE(result.error);
    REQUIRE(result.error->code() == ErrorCode::permission_denied);
    REQUIRE(fixture.provider->calls == 1);
    auto snapshot = fixture.runtime->status(retry_caller(), run);
    REQUIRE(snapshot);
    auto child = std::find_if(snapshot.value().jobs.begin(), snapshot.value().jobs.end(),
                              [&](const auto &value) { return value.id == waiting.id; });
    REQUIRE(child != snapshot.value().jobs.end());
    REQUIRE(child->state == JobState::failed);
    REQUIRE(child->deadline.time() == 100ms);
}

TEST_CASE("B-RETRY01 Runtime workload deadline remains absolute through backoff") {
    RetryFixture fixture;
    const auto run = fixture.submit();
    auto waiting = fixture.await_backoff(run);
    fixture.clock->at(100ms);
    auto result = fixture.finish(run);
    REQUIRE(result.error);
    REQUIRE((result.error->code() == ErrorCode::run_timeout ||
             result.error->code() == ErrorCode::job_timeout));
    REQUIRE(fixture.provider->calls == 1);
    REQUIRE(waiting.deadline.time() == 100ms);
}

TEST_CASE(
    "A25 B-RETRY01 Runtime never retries unknown non-idempotent outcome despite transient claim") {
    RetryFixture fixture(true);
    const auto run = fixture.submit();
    auto result = fixture.finish(run);
    REQUIRE(result.error);
    REQUIRE(result.error->code() == ErrorCode::outcome_unknown);
    REQUIRE(fixture.provider->calls == 1);
    auto snapshot = fixture.runtime->status(retry_caller(), run);
    REQUIRE(snapshot);
    for (const auto &job : snapshot.value().jobs)
        if (job.id != snapshot.value().root)
            REQUIRE(job.attempt == 1);
}
