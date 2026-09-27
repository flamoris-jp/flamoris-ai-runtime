#include "catch_amalgamated.hpp"
#include "flamoris/runtime/native_registration.hpp"
#include "flamoris/runtime/runtime.hpp"
#include <atomic>
#include <mutex>

using namespace flamoris::runtime;
namespace {
class NativeClock final : public MonotonicClock {
  public:
    TimePoint now() const noexcept override { return TimePoint{0}; }
};
class NativeHost final : public HostAuthorityPort {
    std::mutex mutex_;
    std::vector<ResourceTicket> acquired_;
    std::vector<RuntimeHostRelease> released_;

  public:
    std::atomic<unsigned> acquisitions{0};
    Result<void> request_envelope(LogicalResourceId, RuntimeInstanceId) override {
        return Result<void>::success();
    }
    Result<void> acquire(const ResourceTicket &ticket, const ResourceVector &, TimePoint) override {
        std::lock_guard lock(mutex_);
        acquired_.push_back(ticket);
        ++acquisitions;
        return Result<void>::success();
    }
    Result<void> release(OperationId operation, const ResourceTicket &ticket,
                         OperationId grant) override {
        std::lock_guard lock(mutex_);
        released_.push_back({operation, ticket, grant});
        return Result<void>::success();
    }
    Result<void> reconcile(OperationId, const ResourceTicket &, OperationId) override {
        return Result<void>::success();
    }
    bool pending_acquisition() {
        std::lock_guard lock(mutex_);
        return !acquired_.empty();
    }
    void acknowledge(RuntimeInstance &runtime, bool release = true) {
        std::vector<ResourceTicket> acquire;
        std::vector<RuntimeHostRelease> released;
        {
            std::lock_guard lock(mutex_);
            acquire.swap(acquired_);
            if (release)
                released.swap(released_);
        }
        for (const auto &ticket : acquire)
            REQUIRE(runtime.observe_host_acquisition(
                {ticket, runtime.instance_id(), ticket.operation, HostOutcome::acknowledged}));
        for (const auto &receipt : released)
            REQUIRE(runtime.observe_host_release(receipt));
    }
};
struct NativeBarrier {
    std::atomic<bool> armed{true}, blocked{false}, release{false};
    std::atomic<bool> hold_load{false}, load_blocked{false}, release_load{false};
    std::atomic<std::size_t> steps{0}, loads{0}, injections{0};
    std::atomic<std::uintptr_t> model_identity{0};
    std::atomic<bool> shared_model{false};
};
class NativeChild final : public RegisteredProviderPort {
  public:
    std::atomic<unsigned> calls{0};
    ExternalOutcome invoke(std::string_view, const JsonValue &, std::uint64_t,
                           BoundedProviderSink &sink) override {
        ++calls;
        return sink.append("\"child\"") ? ExternalOutcome::confirmed_success
                                        : ExternalOutcome::confirmed_failure;
    }
    ExternalOutcome reconcile(std::string_view, std::uint64_t) override {
        return ExternalOutcome::unknown;
    }
};
// Arithmetic and native state are the production worker. Only delivery of one
// completed token receipt is held, so actor races require no timing assumptions.
class HeldNativeWorker final : public NativeWorkerPort {
    std::unique_ptr<ThreadNativeWorker> worker_;
    std::shared_ptr<NativeBarrier> barrier_;
    std::optional<NativeObservation> held_;

  public:
    HeldNativeWorker(std::unique_ptr<ThreadNativeWorker> worker,
                     std::shared_ptr<NativeBarrier> barrier)
        : worker_(std::move(worker)), barrier_(std::move(barrier)) {}
    Result<void> submit(NativeCommand command) override {
        if (command.operation == NativeOperation::step)
            ++barrier_->steps;
        if (command.operation == NativeOperation::load)
            ++barrier_->loads;
        if (command.operation == NativeOperation::inject)
            ++barrier_->injections;
        return worker_->submit(std::move(command));
    }
    std::optional<NativeObservation> take() override {
        if (held_) {
            if (!(held_->operation == NativeOperation::load ? barrier_->release_load.load()
                                                            : barrier_->release.load()))
                return {};
            if (held_->operation == NativeOperation::step) {
                barrier_->release = false;
                barrier_->blocked = false;
            }
            return std::exchange(held_, {});
        }
        auto receipt = worker_->take();
        if (receipt && receipt->operation == NativeOperation::load && receipt->resident_model) {
            barrier_->model_identity =
                reinterpret_cast<std::uintptr_t>(receipt->resident_model.get());
            if (barrier_->hold_load) {
                held_ = std::move(receipt);
                barrier_->load_blocked = true;
                return {};
            }
        }
        if (receipt && receipt->operation == NativeOperation::step && receipt->segment.token &&
            !receipt->segment.complete && barrier_->armed.exchange(false)) {
            held_ = std::move(receipt);
            barrier_->blocked = true;
            return {};
        }
        return receipt;
    }
    bool idle() const noexcept override { return !held_ && worker_->idle(); }
    Result<void> close() override { return worker_->close(); }
};
AuthorizationContext native_caller() {
    AuthorizationContext caller;
    caller.subject = "native-owner";
    caller.expires_at_ms = 60000;
    caller.capabilities = {"model.native", "algorithm.child"};
    caller.permitted_effects = 63;
    caller.access = {AccessSurface::status, AccessSurface::result, AccessSurface::events,
                     AccessSurface::replay, AccessSurface::cancel, AccessSurface::resume,
                     AccessSurface::inject};
    return caller;
}
std::string native_submission(std::string key = "native") {
    return "{\"schema_version\":\"flamoris.submit/"
           "1\",\"kind\":\"inference\",\"idempotency_key\":\"" +
           key +
           "\",\"inference\":{\"type\":\"model.native\",\"with\":{\"text\":\"日本語😀\"},"
           "\"limits\":{}}}";
}
struct NativeRuntimeFixture {
    std::shared_ptr<NativeHost> host = std::make_shared<NativeHost>();
    std::shared_ptr<NativeBarrier> barrier = std::make_shared<NativeBarrier>();
    std::shared_ptr<NativeBarrier> second_barrier = std::make_shared<NativeBarrier>();
    std::shared_ptr<NativeChild> child = std::make_shared<NativeChild>();
    NativeWorkerConfig recipe;
    std::unique_ptr<RuntimeInstance> runtime;
    explicit NativeRuntimeFixture(std::uint64_t instance = 711, bool opencl = false,
                                  bool dynamic = false, bool sharing = false,
                                  bool scoped_child = false) {
        NativeWorkerConfig native;
        native.registered_artifact =
            std::filesystem::path(FLAMORIS_SOURCE_DIR) / "fixtures/native/tiny-causal-v1.bin";
        native.artifact_sha256 = "5cd5a0bacb4cce1efd801fe2a4d7c1347467538ef6e393b37d955d2499ef74e2";
        native.options.sampling.literal_grammar = "Hi😀";
        native.options.max_output_tokens = 12;
        if (dynamic) {
            native.options.sampling.literal_grammar.clear();
            native.options.injection_slot = true;
        }
        native.opencl = opencl;
        if (opencl) {
            auto devices = enumerate_opencl_devices();
            REQUIRE(devices);
            REQUIRE_FALSE(devices.value().empty());
            native.compute_identity = devices.value().front().identity;
            native.retained_context_bytes = 1048576;
        }
        CapabilityContract capability(EffectSet::from_mask(1).value());
        capability.identifier = "model.native";
        capability.version = "1";
        capability.adapter_revision = "native/1";
        capability.inference = capability.pausable = capability.cancellable = true;
        capability.native_pins = expected_native_pins(native).value();
        ValueSchema text;
        text.kind = ValueSchema::Kind::string;
        text.max_bytes = 255;
        capability.input_schema.kind = ValueSchema::Kind::object;
        capability.input_schema.properties["text"] = text;
        capability.input_schema.required.insert("text");
        capability.output_schema = text;
        RuntimeConfiguration config;
        config.instance = {instance, 1};
        config.clock = std::make_shared<NativeClock>();
        config.host = host;
        config.capabilities.capabilities.emplace(capability.identifier, capability);
        config.policy.capabilities = native_caller().capabilities;
        config.policy.permitted_effects = 63;
        config.policy.access = native_caller().access;
        if (scoped_child)
            config.policy.object_scopes = {"object.a", "object.b"};
        auto gate = barrier;
        auto second_gate = second_barrier;
        auto count = std::make_shared<std::atomic<unsigned>>(0);
        barrier->hold_load = sharing;
        config.native_worker_factory =
            [gate, second_gate, count, sharing](
                NativeWorkerConfig worker_config) -> Result<std::unique_ptr<NativeWorkerPort>> {
            auto selected_gate = sharing && count->fetch_add(1) != 0 ? second_gate : gate;
            selected_gate->shared_model = static_cast<bool>(worker_config.shared_model);
            auto worker = ThreadNativeWorker::create(std::move(worker_config));
            if (!worker)
                return Result<std::unique_ptr<NativeWorkerPort>>::failure(worker.error());
            return Result<std::unique_ptr<NativeWorkerPort>>::success(
                std::make_unique<HeldNativeWorker>(std::move(worker).value(), selected_gate));
        };
        RuntimeRegistration registration;
        registration.capability = capability.identifier;
        registration.native = native;
        registration.prompt_field = "text";
        registration.requirements[ResourceKind::execution] = 1;
        registration.requirements[ResourceKind::ram] = 4194304;
        registration.run_resource_limit.values.fill(16777216);
        if (opencl) {
            registration.requirements[ResourceKind::device] = native.device_memory_bound;
            registration.requirements[ResourceKind::transfer] = native.device_memory_bound;
        }
        config.registrations.push_back(registration);
        if (dynamic) {
            config.max_workers = 1;
            auto child_capability = capability;
            child_capability.identifier = "algorithm.child";
            child_capability.inference = child_capability.pausable = false;
            child_capability.native_pins.clear();
            if (scoped_child)
                child_capability.object_scope_fields = {"text"};
            config.capabilities.capabilities.emplace(child_capability.identifier, child_capability);
            ChildEnvelope envelope;
            envelope.identifier = "native.children";
            envelope.revision = "1";
            envelope.capabilities.push_back(fingerprint_capability(child_capability).value());
            envelope.max_children = 2;
            envelope.max_depth = 1;
            envelope.max_attempts = 2;
            envelope.max_output_bytes = 255;
            config.capabilities.child_policies.emplace(envelope.identifier, envelope);
            RuntimeRegistration child_registration;
            child_registration.capability = child_capability.identifier;
            child_registration.provider = child;
            child_registration.requirements[ResourceKind::execution] = 1;
            child_registration.requirements[ResourceKind::adapter] = 1;
            child_registration.run_resource_limit.values.fill(16777216);
            config.registrations.push_back(child_registration);
        }
        recipe = native;
        auto created = RuntimeInstance::create(std::move(config));
        REQUIRE(created);
        runtime = std::move(created).value();
        HostEnvelope envelope;
        envelope.instance = runtime->instance_id();
        envelope.resource = {1};
        envelope.epoch = HostEpoch{1};
        envelope.authority_revision = 1;
        envelope.expires = std::chrono::seconds(60);
        envelope.capacity.values.fill(16777216);
        envelope.arbitration = HostArbitration::enforced_generation;
        envelope.contract_verified = envelope.reconciled_inventory = true;
        REQUIRE(runtime->observe_host_envelope(envelope));
    }
    void poll(bool release = true) {
        host->acknowledge(*runtime, release);
        REQUIRE(runtime->poll());
    }
    template <class Predicate> void until(Predicate predicate, bool release = true) {
        for (unsigned i = 0; i < 20000; ++i) {
            poll(release);
            if (predicate())
                return;
        }
        FAIL("bounded actor progress did not reach the explicit native barrier");
    }
    RunId submit() {
        auto submitted = runtime->submit(native_caller(), native_submission());
        REQUIRE(submitted);
        return submitted.value().id;
    }
    RunResult finish(RunId id) {
        RunResult result;
        until([&] {
            auto observed = runtime->result(native_caller(), id);
            REQUIRE(observed);
            result = observed.value();
            return !result.pending;
        });
        return result;
    }
};
} // namespace
TEST_CASE("C12 B-REAL02 real CPU Runtime preserves a pending token through pause and resume",
          "[native][runtime]") {
    NativeRuntimeFixture fixture;
    auto id = fixture.submit();
    fixture.until([&] { return fixture.barrier->blocked.load(); });
    REQUIRE(fixture.runtime->pause(native_caller(), id, 41));
    auto barrier = fixture.runtime->status(native_caller(), id);
    REQUIRE(barrier);
    REQUIRE(barrier.value().pause_barrier_pending);
    fixture.barrier->release = true;
    fixture.until([&] {
        auto status = fixture.runtime->status(native_caller(), id);
        REQUIRE(status);
        return !status.value().pause_barrier_pending &&
               status.value().jobs.front().state == JobState::paused;
    });
    const auto steps = fixture.barrier->steps.load();
    for (int i = 0; i < 16; ++i)
        fixture.poll();
    REQUIRE(fixture.barrier->steps == steps);
    auto resources = fixture.runtime->resource_snapshot({1});
    REQUIRE(resources);
    REQUIRE(resources.value().resident[ResourceKind::ram] > 0);
    REQUIRE(resources.value().executing.empty());
    REQUIRE(fixture.runtime->resume(native_caller(), id, 41));
    auto result = fixture.finish(id);
    if (result.error)
        WARN(to_string(result.error->code()));
    REQUIRE_FALSE(result.error);
    REQUIRE(result.value == JsonValue{"Hi😀"});
    REQUIRE(fixture.barrier->loads == 1);
}
TEST_CASE(
    "C12 delayed actual CPU token cannot publish after cancellation or settle without host ack",
    "[native][runtime]") {
    NativeRuntimeFixture fixture;
    auto id = fixture.submit();
    fixture.until([&] { return fixture.barrier->blocked.load(); });
    REQUIRE(fixture.runtime->cancel(native_caller(), id));
    fixture.barrier->release = true;
    fixture.until(
        [&] {
            auto status = fixture.runtime->status(native_caller(), id);
            REQUIRE(status);
            return status.value().jobs.front().state == JobState::finalizing;
        },
        false);
    auto pending = fixture.runtime->result(native_caller(), id);
    REQUIRE(pending);
    REQUIRE(pending.value().pending);
    auto resources = fixture.runtime->resource_snapshot({1});
    REQUIRE(resources);
    REQUIRE_FALSE(resources.value().executing.empty());
    auto result = fixture.finish(id);
    REQUIRE(result.error);
    REQUIRE_FALSE(result.value);
    auto final_resources = fixture.runtime->resource_snapshot({1});
    REQUIRE(final_resources);
    REQUIRE(final_resources.value().resident.empty());
    REQUIRE(final_resources.value().executing.empty());
}
TEST_CASE("C12 actual native CPU state and IDs remain independent across Runtime instances",
          "[native][runtime]") {
    NativeRuntimeFixture first(811), second(812);
    auto first_id = first.submit(), second_id = second.submit();
    first.until([&] { return first.barrier->blocked.load(); });
    second.barrier->release = true;
    auto second_result = second.finish(second_id);
    REQUIRE_FALSE(second_result.error);
    REQUIRE(second_result.value == JsonValue{"Hi😀"});
    REQUIRE(first.runtime->result(native_caller(), first_id).value().pending);
    REQUIRE_FALSE(second.runtime->status(native_caller(), first_id));
    first.barrier->release = true;
    REQUIRE(first.finish(first_id).value == second_result.value);
}
TEST_CASE("C12 B-INPUT01 real native parent yields to compiled child and injects its result once",
          "[native][runtime]") {
    NativeRuntimeFixture fixture(912, false, true);
    auto request = native_submission();
    request.insert(request.find("\"limits\""), "\"child_policy\":\"native.children\",");
    auto submitted = fixture.runtime->submit(native_caller(), request);
    REQUIRE(submitted);
    auto id = submitted.value().id;
    fixture.until([&] { return fixture.barrier->blocked.load(); });
    auto snapshot = fixture.runtime->status(native_caller(), id);
    REQUIRE(snapshot);
    const auto root = snapshot.value().root;
    REQUIRE_FALSE(fixture.runtime->propose_children(native_caller(), id, root, "{}"));
    const auto fragment =
        R"({"schema_version":"flamoris.submit/1","kind":"workflow","workflow":{"schema_version":"flamoris.workflow/0.1","workflow":{"id":"child"},"inputs":{},"nodes":[{"id":"call","type":"algorithm.child","with":{"text":{"literal":"x"}}}],"edges":[],"outputs":{"value":{"ref":{"source":"node","name":"call","path":[]}}},"limits":{}},"input_values":{}})";
    auto proposed = fixture.runtime->propose_children(native_caller(), id, root, fragment);
    if (!proposed)
        WARN(to_string(proposed.error().code()));
    REQUIRE(proposed);
    fixture.barrier->release = true;
    auto result = fixture.finish(id);
    if (result.error) {
        WARN(to_string(result.error->code()));
        auto failed_state = fixture.runtime->status(native_caller(), id);
        REQUIRE(failed_state);
        for (const auto &job : failed_state.value().jobs)
            WARN("job " << job.id.value() << " error "
                        << (job.error ? to_string(*job.error) : "none"));
        WARN("child calls=" << fixture.child->calls
                            << " injections=" << fixture.barrier->injections);
    }
    REQUIRE_FALSE(result.error);
    REQUIRE(fixture.child->calls == 1);
    REQUIRE(fixture.barrier->injections == 1);
    REQUIRE(fixture.barrier->loads == 1);
    auto final_state = fixture.runtime->status(native_caller(), id);
    REQUIRE(final_state);
    REQUIRE(final_state.value().jobs.size() == 3);
    for (const auto &job : final_state.value().jobs)
        REQUIRE(job.state == JobState::succeeded);
    auto weights =
        TinyModel::load(fixture.recipe.registered_artifact, fixture.recipe.artifact_sha256);
    REQUIRE(weights);
    auto oracle = NativeSession::create(weights.value(), make_cpu_compute(), {}, {}, "日本語😀",
                                        fixture.recipe.options);
    REQUIRE(oracle);
    while (oracle.value()->state().stage == NativeStage::prefill)
        REQUIRE(oracle.value()->step());
    REQUIRE(oracle.value()->step());
    auto paused = oracle.value()->pause();
    REQUIRE(paused);
    REQUIRE(oracle.value()->resume(oracle.value()->state().pins, paused.value()));
    REQUIRE(oracle.value()->inject("{\"value\":\"child\"}", 1));
    while (oracle.value()->state().stage != NativeStage::completing)
        REQUIRE(oracle.value()->step());
    REQUIRE(result.value == JsonValue{oracle.value()->causal_state().output});
    REQUIRE(oracle.value()->release());
}
TEST_CASE("A24 proposed child retains the admitted Run object-scope ceiling", "[native][runtime]") {
    NativeRuntimeFixture fixture(913, false, true, false, true);
    auto narrow = native_caller();
    narrow.object_scopes = {"object.a"};
    auto broad = narrow;
    broad.object_scopes.insert("object.b");
    auto request = native_submission("scoped-child");
    request.insert(request.find("\"limits\""), "\"child_policy\":\"native.children\",");
    auto submitted = fixture.runtime->submit(narrow, request);
    REQUIRE(submitted);
    fixture.until([&] { return fixture.barrier->blocked.load(); });
    const auto root = fixture.runtime->status(narrow, submitted.value().id).value().root;
    const auto fragment =
        R"({"schema_version":"flamoris.submit/1","kind":"workflow","workflow":{"schema_version":"flamoris.workflow/0.1","workflow":{"id":"child"},"inputs":{},"nodes":[{"id":"call","type":"algorithm.child","with":{"text":{"literal":"object.b"}}}],"edges":[],"outputs":{"value":{"ref":{"source":"node","name":"call","path":[]}}},"limits":{}},"input_values":{}})";
    auto proposed = fixture.runtime->propose_children(broad, submitted.value().id, root, fragment);
    REQUIRE_FALSE(proposed);
    REQUIRE(proposed.error().code() == ErrorCode::permission_denied);
    fixture.barrier->release = true;
    REQUIRE_FALSE(fixture.finish(submitted.value().id).error);
    REQUIRE(fixture.child->calls == 0);
    REQUIRE(fixture.runtime->status(narrow, submitted.value().id).value().jobs.size() == 1);
}
TEST_CASE("A05 exhausted attempts cannot activate an acknowledged child lease",
          "[native][runtime]") {
    NativeRuntimeFixture fixture(914, false, true);
    auto request = native_submission("attempt-bound");
    request.insert(request.find("\"limits\""), "\"child_policy\":\"native.children\",");
    request.replace(request.find("\"limits\":{}"), sizeof("\"limits\":{}") - 1,
                    "\"limits\":{\"max_attempts\":4}");
    auto admitted = fixture.runtime->submit(native_caller(), request);
    REQUIRE(admitted);
    const auto id = admitted.value().id;
    const auto fragment =
        R"({"schema_version":"flamoris.submit/1","kind":"workflow","workflow":{"schema_version":"flamoris.workflow/0.1","workflow":{"id":"child"},"inputs":{},"nodes":[{"id":"call","type":"algorithm.child","with":{"text":{"literal":"x"}}}],"edges":[],"outputs":{"value":{"ref":{"source":"node","name":"call","path":[]}}},"limits":{}},"input_values":{}})";
    fixture.until([&] { return fixture.barrier->blocked.load(); });
    const auto root = fixture.runtime->status(native_caller(), id).value().root;
    REQUIRE(fixture.runtime->propose_children(native_caller(), id, root, fragment));
    fixture.barrier->armed = true;
    fixture.barrier->release = true;
    fixture.until([&] {
        return fixture.child->calls == 1 && fixture.barrier->blocked && !fixture.barrier->release;
    });
    REQUIRE(fixture.runtime->propose_children(native_caller(), id, root, fragment));
    fixture.barrier->release = true;
    bool pending = false;
    for (unsigned turn = 0; turn < 20000; ++turn) {
        fixture.poll();
        pending = fixture.host->pending_acquisition();
        if (pending)
            break;
    }
    REQUIRE(pending);
    fixture.host->acknowledge(*fixture.runtime, false);
    REQUIRE(fixture.runtime->poll());
    auto ledger = fixture.runtime->resource_snapshot({1});
    REQUIRE(ledger);
    REQUIRE(ledger.value().executing.empty());
    REQUIRE(fixture.child->calls == 1);
    auto jobs = fixture.runtime->status(native_caller(), id);
    REQUIRE(jobs);
    REQUIRE(jobs.value().jobs.size() == 5);
    const auto denied_job = jobs.value().jobs.back().id;
    auto events = fixture.runtime->events(native_caller(), id, 0, 256);
    REQUIRE(events);
    for (const auto &group : events.value().groups)
        for (const auto &event : group.events)
            REQUIRE_FALSE((event.job == denied_job && event.kind == "attempt.dispatch_committed"));
}
TEST_CASE("C12 A11 actual concurrent native Runs share one load and preserve the last residency",
          "[native][runtime]") {
    NativeRuntimeFixture fixture(922, false, false, true);
    auto first = fixture.submit();
    fixture.until([&] { return fixture.barrier->load_blocked.load(); });
    auto submitted = fixture.runtime->submit(native_caller(), native_submission("second"));
    REQUIRE(submitted);
    auto second = submitted.value().id;
    for (int i = 0; i < 16; ++i)
        fixture.poll();
    REQUIRE(fixture.host->acquisitions == 1); // Shared load wait owns no second execution lease.
    REQUIRE(fixture.second_barrier->loads == 0);
    fixture.barrier->release_load = true;
    fixture.until(
        [&] { return fixture.barrier->blocked.load() && fixture.second_barrier->blocked.load(); });
    REQUIRE_FALSE(fixture.barrier->shared_model);
    REQUIRE(fixture.second_barrier->shared_model);
    REQUIRE(fixture.barrier->model_identity != 0);
    REQUIRE(fixture.barrier->model_identity == fixture.second_barrier->model_identity);
    auto together = fixture.runtime->resource_snapshot({1});
    REQUIRE(together);
    REQUIRE(together.value().resident[ResourceKind::ram] == 25984 + 2 * 131072);
    fixture.barrier->release = true;
    auto first_result = fixture.finish(first);
    REQUIRE_FALSE(first_result.error);
    REQUIRE(first_result.value == JsonValue{"Hi😀"});
    auto remaining = fixture.runtime->resource_snapshot({1});
    REQUIRE(remaining);
    REQUIRE(remaining.value().resident[ResourceKind::ram] == 25984 + 131072);
    REQUIRE(fixture.runtime->result(native_caller(), second).value().pending);
    fixture.second_barrier->release = true;
    auto second_result = fixture.finish(second);
    REQUIRE_FALSE(second_result.error);
    REQUIRE(second_result.value == first_result.value);
    auto released = fixture.runtime->resource_snapshot({1});
    REQUIRE(released);
    REQUIRE(released.value().resident.empty());
    REQUIRE(released.value().executing.empty());
}
#ifdef FLAMORIS_ENABLE_OPENCL
TEST_CASE("C12 B-OPENCL01 actual qualified OpenCL flows through Runtime host accounting",
          "[native][runtime][opencl]") {
    NativeRuntimeFixture fixture(913, true);
    fixture.barrier->release = true;
    auto result = fixture.finish(fixture.submit());
    if (result.error)
        WARN(to_string(result.error->code()));
    REQUIRE_FALSE(result.error);
    REQUIRE(result.value == JsonValue{"Hi😀"});
    auto resources = fixture.runtime->resource_snapshot({1});
    REQUIRE(resources);
    REQUIRE(resources.value().resident.empty());
    REQUIRE(resources.value().executing.empty());
}
#endif
