#include "catch_amalgamated.hpp"
#include "flamoris/runtime/native_registration.hpp"
#include "flamoris/runtime/principal_target.hpp"
#include "flamoris/runtime/runtime.hpp"
#include <atomic>
#include <mutex>

using namespace flamoris::runtime;
namespace {
class AtomicClock final : public MonotonicClock {
  public:
    std::atomic<std::int64_t> time{};
    TimePoint now() const noexcept override { return TimePoint{time.load()}; }
};
class Host final : public HostAuthorityPort {
  public:
    std::mutex mutex;
    std::vector<ResourceTicket> acquisitions;
    std::vector<RuntimeHostRelease> releases;
    std::uint64_t dispatched{};
    Result<void> request_envelope(LogicalResourceId, RuntimeInstanceId) override {
        return Result<void>::success();
    }
    Result<void> acquire(const ResourceTicket &t, const ResourceVector &, TimePoint) override {
        std::lock_guard lock(mutex);
        acquisitions.push_back(t);
        ++dispatched;
        return Result<void>::success();
    }
    Result<void> release(OperationId operation, const ResourceTicket &t,
                         OperationId grant) override {
        std::lock_guard lock(mutex);
        releases.push_back({operation, t, grant});
        return Result<void>::success();
    }
    Result<void> reconcile(OperationId, const ResourceTicket &, OperationId) override {
        return Result<void>::success();
    }
    void respond(RuntimeInstance &runtime, bool acknowledge_release = true) {
        std::vector<ResourceTicket> acquire;
        std::vector<RuntimeHostRelease> release;
        {
            std::lock_guard lock(mutex);
            acquire.swap(acquisitions);
            if (acknowledge_release)
                release.swap(releases);
        }
        for (auto t : acquire)
            REQUIRE(runtime.observe_host_acquisition(
                {t, runtime.instance_id(), t.operation, HostOutcome::acknowledged}));
        for (auto r : release)
            REQUIRE(runtime.observe_host_release(r));
    }
};
class Echo final : public RegisteredProviderPort {
  public:
    std::atomic<unsigned> calls{};
    ExternalOutcome invoke(std::string_view, const JsonValue &input, std::uint64_t,
                           BoundedProviderSink &sink) override {
        ++calls;
        auto text = std::get<JsonValue::Object>(input.data).at("text");
        auto encoded = canonical_json(text);
        if (!encoded || !sink.append(encoded.value()))
            return ExternalOutcome::confirmed_failure;
        return ExternalOutcome::confirmed_success;
    }
    ExternalOutcome reconcile(std::string_view, std::uint64_t) override {
        return ExternalOutcome::unknown;
    }
};
CapabilityContract capability(std::string name, bool native = false) {
    CapabilityContract c(EffectSet::from_mask(1).value());
    c.identifier = std::move(name);
    c.version = "1";
    c.adapter_revision = "registered/1";
    ValueSchema text;
    text.kind = ValueSchema::Kind::string;
    text.max_bytes = 255;
    c.input_schema.kind = ValueSchema::Kind::object;
    c.input_schema.properties["text"] = text;
    c.input_schema.required.insert("text");
    c.output_schema = text;
    c.inference = native;
    c.pausable = native;
    c.cancellable = true;
    if (native)
        c.native_pins = {{"model", "flamoris.tiny-causal.v1"},
                         {"processor", "processor/1"},
                         {"tokenizer", "tokenizer/1"},
                         {"execution_profile", "native/1"},
                         {"compute", "cpu/1"}};
    return c;
}
AuthorizationContext caller() {
    AuthorizationContext c;
    c.subject = "owner";
    c.expires_at_ms = 60000;
    c.capabilities = {"algorithm.echo", "model.native"};
    c.permitted_effects = 63;
    c.access = {AccessSurface::status, AccessSurface::result, AccessSurface::events,
                AccessSurface::replay, AccessSurface::cancel, AccessSurface::resume};
    return c;
}
struct Fixture {
    std::shared_ptr<Host> host = std::make_shared<Host>();
    std::shared_ptr<AtomicClock> clock = std::make_shared<AtomicClock>();
    std::shared_ptr<Echo> echo = std::make_shared<Echo>();
    std::unique_ptr<RuntimeInstance> runtime;
    CapabilitySnapshot registry;
    explicit Fixture(bool native = false, std::function<void(PendingSubmissionId, bool)> hook = {},
                     RuntimeInstanceId instance = {99, 1}) {
        RuntimeConfiguration config;
        config.instance = instance;
        config.submission_preparation_hook = std::move(hook);
        config.host = host;
        config.clock = clock;
        auto cap = capability(native ? "model.native" : "algorithm.echo", native);
        registry.capabilities.emplace(cap.identifier, cap);
        config.capabilities = registry;
        config.policy.capabilities = caller().capabilities;
        config.policy.permitted_effects = 63;
        config.policy.access = caller().access;
        RuntimeRegistration r;
        r.capability = cap.identifier;
        r.requirements[ResourceKind::execution] = 1;
        r.run_resource_limit[ResourceKind::execution] = 4;
        if (native) {
            NativeWorkerConfig n;
            n.registered_artifact =
                std::filesystem::path(FLAMORIS_SOURCE_DIR) / "fixtures/native/tiny-causal-v1.bin";
            n.artifact_sha256 = "5cd5a0bacb4cce1efd801fe2a4d7c1347467538ef6e393b37d955d2499ef74e2";
            n.options.max_output_tokens = 4;
            n.options.sampling.literal_grammar = "Hi";
            r.native = n;
            registry.capabilities.at(cap.identifier).native_pins = expected_native_pins(n).value();
            config.capabilities = registry;
            r.prompt_field = "text";
            r.requirements[ResourceKind::ram] = 1048576;
            r.run_resource_limit[ResourceKind::ram] = 4194304;
        } else {
            r.provider = echo;
            r.requirements[ResourceKind::adapter] = 1;
            r.run_resource_limit[ResourceKind::adapter] = 4;
        }
        config.registrations.push_back(r);
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
        envelope.expires = std::chrono::seconds(60);
        envelope.capacity.values.fill(4194304);
        envelope.arbitration = HostArbitration::enforced_generation;
        envelope.contract_verified = true;
        envelope.reconciled_inventory = true;
        REQUIRE(runtime->observe_host_envelope(envelope));
    }
    RunResult finish(RunId id, bool release = true) {
        for (unsigned i = 0; i < 10000; ++i) {
            host->respond(*runtime, release);
            REQUIRE(runtime->poll());
            auto result = runtime->result(caller(), id);
            REQUIRE(result);
            if (!result.value().pending)
                return result.value();
        }
        auto state = runtime->status(caller(), id);
        for (const auto &job : state.value().jobs)
            WARN("job=" << job.id.value() << " state=" << static_cast<int>(job.state)
                        << " error=" << (job.error ? to_string(*job.error) : "none"));
        WARN("echo calls=" << echo->calls.load() << " host=" << host->dispatched);
        FAIL("bounded actor progress did not finish");
        return {};
    }
};
std::string workflow() {
    return R"({"schema_version":"flamoris.submit/1","kind":"workflow","idempotency_key":"same","workflow":{"schema_version":"flamoris.workflow/0.1","workflow":{"id":"test"},"inputs":{},"nodes":[{"id":"a","type":"algorithm.echo","with":{"text":{"literal":"hello"}}}],"edges":[],"outputs":{"result":{"ref":{"source":"node","name":"a","path":[]}}},"limits":{}},"input_values":{}})";
}
std::string inference() {
    return R"({"schema_version":"flamoris.submit/1","kind":"inference","idempotency_key":"native","inference":{"type":"model.native","with":{"text":"test"},"limits":{}}})";
}
} // namespace
TEST_CASE(
    "C12 actor composes registered workflow and current observation without implicit host grant") {
    Fixture f;
    auto admitted = f.runtime->submit(caller(), workflow());
    REQUIRE(admitted);
    for (unsigned i = 0; i < 10; ++i)
        REQUIRE(f.runtime->poll());
    REQUIRE(f.echo->calls == 0);
    f.envelope();
    auto result = f.finish(admitted.value().id);
    if (result.error)
        WARN(to_string(result.error->code()));
    REQUIRE_FALSE(result.error);
    REQUIRE(result.value == JsonValue{JsonValue::Object{{"result", "hello"}}});
    REQUIRE(f.echo->calls == 1);
    auto replay = f.runtime->replay(caller(), admitted.value().id);
    if (!replay)
        WARN(to_string(replay.error().code()));
    REQUIRE(replay);
    REQUIRE(replay.value().complete);
    auto changed = f.registry;
    changed.capabilities.at("algorithm.echo").adapter_revision = "2";
    REQUIRE(f.runtime->replace_capabilities(changed));
    auto duplicate = f.runtime->submit(caller(), workflow());
    REQUIRE(duplicate);
    REQUIRE(duplicate.value().duplicate);
    REQUIRE(duplicate.value().id == admitted.value().id);
    REQUIRE(f.echo->calls == 1);
}
TEST_CASE("C12 native compiler actor worker resource receipts produce a single root result") {
    Fixture f(true);
    f.envelope();
    auto admitted = f.runtime->submit(caller(), inference());
    REQUIRE(admitted);
    auto result = f.finish(admitted.value().id);
    if (result.error)
        WARN(to_string(result.error->code()));
    REQUIRE_FALSE(result.error);
    REQUIRE(result.value == JsonValue{"Hi"});
    auto state = f.runtime->status(caller(), admitted.value().id);
    REQUIRE(state);
    REQUIRE(state.value().jobs.size() == 1);
    REQUIRE(state.value().jobs.front().state == JobState::succeeded);
    auto resources = f.runtime->resource_snapshot({1});
    REQUIRE(resources);
    REQUIRE(resources.value().resident.empty());
    REQUIRE(resources.value().executing.empty());
}
TEST_CASE("C12 missing host release preserves finalizing and charged lease") {
    Fixture f;
    f.envelope();
    auto admitted = f.runtime->submit(caller(), workflow());
    REQUIRE(admitted);
    for (unsigned i = 0; i < 300; ++i) {
        f.host->respond(*f.runtime, false);
        REQUIRE(f.runtime->poll());
    }
    REQUIRE(f.echo->calls == 1);
    auto result = f.runtime->result(caller(), admitted.value().id);
    REQUIRE(result);
    REQUIRE(result.value().pending);
    auto resource = f.runtime->resource_snapshot({1});
    REQUIRE(resource);
    REQUIRE(resource.value().executing[ResourceKind::execution] == 1);
    REQUIRE_FALSE(f.finish(admitted.value().id).pending);
}

TEST_CASE("C12 pinned Runtime admission rejects recompilation target and concrete input changes") {
    Fixture f;
    auto snapshot = f.runtime->compilation_snapshot();
    REQUIRE(snapshot);
    auto compiled = Compiler(snapshot.value().compiler)
                        .compile_submission(workflow(), snapshot.value().capabilities);
    REQUIRE(compiled);
    PinnedSubmissionIdentity expected{snapshot.value().instance, compiled.value().request_digest,
                                      compiled.value().plan->fingerprint};
    auto other = expected;
    other.instance = {99, 2};
    auto rejected = f.runtime->submit_pinned(caller(), workflow(), other);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code() == ErrorCode::plan_stale);
    auto wire = workflow();
    wire.replace(wire.find("hello"), 5, "changed");
    REQUIRE_FALSE(f.runtime->submit_pinned(caller(), wire, expected));
    auto changed = snapshot.value().capabilities;
    changed.capabilities.at("algorithm.echo").adapter_revision = "registered/2";
    REQUIRE(f.runtime->replace_capabilities(changed));
    rejected = f.runtime->submit_pinned(caller(), workflow(), expected);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code() == ErrorCode::plan_stale);
    REQUIRE(f.echo->calls == 0);
    auto fresh = f.runtime->compilation_snapshot();
    REQUIRE(fresh);
    auto recompiled =
        Compiler(fresh.value().compiler).compile_submission(workflow(), fresh.value().capabilities);
    REQUIRE(recompiled);
    expected.plan_fingerprint = recompiled.value().plan->fingerprint;
    REQUIRE(f.runtime->submit_pinned(caller(), workflow(), expected));
    REQUIRE(f.echo->calls == 0);
}
TEST_CASE(
    "pinned duplicate receipt checks the existing Run rather than substituting its identity") {
    Fixture f;
    auto snapshot = f.runtime->compilation_snapshot();
    REQUIRE(snapshot);
    auto compiled = Compiler(snapshot.value().compiler)
                        .compile_submission(workflow(), snapshot.value().capabilities);
    REQUIRE(compiled);
    PinnedSubmissionIdentity expected{snapshot.value().instance, compiled.value().request_digest,
                                      compiled.value().plan->fingerprint};
    auto first = f.runtime->submit_pinned(caller(), workflow(), expected);
    REQUIRE(first);
    auto duplicate = f.runtime->submit_pinned(caller(), workflow(), expected);
    REQUIRE(duplicate);
    REQUIRE(duplicate.value().duplicate);
    REQUIRE(duplicate.value().id == first.value().id);
    expected.plan_fingerprint = std::string(64, '0');
    REQUIRE_FALSE(f.runtime->submit_pinned(caller(), workflow(), expected));
    REQUIRE(f.echo->calls == 0);
}

TEST_CASE("C12 pinned admission rechecks contracts changed after submission preparation") {
    RuntimeInstance *runtime = nullptr;
    CapabilitySnapshot replacement;
    bool changed = false;
    Fixture f(false, [&](PendingSubmissionId, bool owner) {
        if (owner && !changed) {
            changed = true;
            REQUIRE(runtime->replace_capabilities(replacement));
        }
    });
    runtime = f.runtime.get();
    auto snapshot = runtime->compilation_snapshot();
    REQUIRE(snapshot);
    replacement = snapshot.value().capabilities;
    replacement.capabilities.at("algorithm.echo").adapter_revision = "raced/2";
    auto compiled = Compiler(snapshot.value().compiler)
                        .compile_submission(workflow(), snapshot.value().capabilities);
    REQUIRE(compiled);
    PinnedSubmissionIdentity expected{snapshot.value().instance, compiled.value().request_digest,
                                      compiled.value().plan->fingerprint};
    auto receipt = runtime->submit_pinned(caller(), workflow(), expected);
    REQUIRE(changed);
    REQUIRE_FALSE(receipt);
    REQUIRE(receipt.error().code() == ErrorCode::plan_stale);
    REQUIRE(f.echo->calls == 0);
}

TEST_CASE("C12 isolated principal targets deny other subjects across every command surface") {
    Fixture f;
    auto created = PrincipalRuntimeTarget::create("owner", std::move(f.runtime));
    REQUIRE(created);
    auto &target = *created.value();
    auto other = caller();
    other.subject = "other";
    auto denied = [](const auto &result) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code() == ErrorCode::permission_denied);
    };
    denied(target.submit(other, workflow()));
    auto admitted = target.submit(caller(), workflow());
    REQUIRE(admitted);
    const auto id = admitted.value().id;
    denied(target.status(other, id));
    denied(target.result(other, id));
    denied(target.cancel(other, id));
    denied(target.pause(other, id, 1));
    denied(target.resume(other, id, 2));
    denied(target.events(other, id, 0, 16));
    denied(target.replay(other, id));
    REQUIRE(target.status(caller(), id));
    auto snapshot = target.trusted_runtime().compilation_snapshot();
    REQUIRE(snapshot);
    auto compiled = Compiler(snapshot.value().compiler)
                        .compile_submission(workflow(), snapshot.value().capabilities);
    REQUIRE(compiled);
    PinnedSubmissionIdentity pin{snapshot.value().instance, compiled.value().request_digest,
                                 compiled.value().plan->fingerprint};
    denied(target.submit_pinned(other, workflow(), pin));
    auto revoked = caller();
    revoked.revoked = true;
    REQUIRE_FALSE(target.submit(revoked, workflow()));
    REQUIRE_FALSE(target.status(revoked, id));
    auto expired = caller();
    expired.expires_at_ms = 0;
    REQUIRE_FALSE(target.submit(expired, workflow()));
    REQUIRE_FALSE(target.result(expired, id));
    REQUIRE(f.echo->calls == 0);
}
TEST_CASE(
    "C12 target ownership requires separate Runtime instances and preserves scoped deduplication") {
    Fixture a;
    Fixture b(false, {}, {99, 2});
    auto first = PrincipalRuntimeTarget::create("owner", std::move(a.runtime));
    auto second = PrincipalRuntimeTarget::create("other", std::move(b.runtime));
    REQUIRE(first);
    REQUIRE(second);
    auto other = caller();
    other.subject = "other";
    auto left = first.value()->submit(caller(), workflow());
    auto right = second.value()->submit(other, workflow());
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(left.value().id.instance() != right.value().id.instance());
    REQUIRE_FALSE(left.value().duplicate);
    REQUIRE_FALSE(right.value().duplicate);
    REQUIRE_FALSE(first.value()->status(other, right.value().id));
    REQUIRE_FALSE(second.value()->status(caller(), left.value().id));
    REQUIRE(first.value()->status(caller(), left.value().id));
    REQUIRE(second.value()->status(other, right.value().id));
    auto duplicate = second.value()->submit(other, workflow());
    REQUIRE(duplicate);
    REQUIRE(duplicate.value().duplicate);
    REQUIRE_FALSE(PrincipalRuntimeTarget::create("owner", nullptr));
    Fixture invalid;
    REQUIRE_FALSE(
        PrincipalRuntimeTarget::create(std::string(129, 'x'), std::move(invalid.runtime)));
    Fixture portable;
    REQUIRE(PrincipalRuntimeTarget::create("authenticated identity with spaces",
                                           std::move(portable.runtime)));
}
