#include "catch_amalgamated.hpp"
#include "flamoris/runtime/authorization.hpp"
#include "flamoris/runtime/submission.hpp"
using namespace flamoris::runtime;
namespace {
const std::string digest(64, 'a');
CapabilityContract cap() {
    CapabilityContract c(EffectSet::from_mask(1).value());
    c.identifier = "algorithm.pure";
    c.version = "1";
    c.adapter_revision = "1";
    c.input_schema.kind = ValueSchema::Kind::object;
    return c;
}
AuthorizationContext context() {
    AuthorizationContext c;
    c.subject = "local";
    c.expires_at_ms = 100;
    c.capabilities = {"algorithm.pure"};
    c.object_scopes = {"object.a"};
    c.permitted_effects = 63;
    c.access = {AccessSurface::status, AccessSurface::result, AccessSurface::handle};
    return c;
}
PolicySnapshot policy() {
    PolicySnapshot p;
    p.capabilities = {"algorithm.pure"};
    p.object_scopes = {"object.a"};
    p.permitted_effects = 63;
    p.access = {AccessSurface::status, AccessSurface::result, AccessSurface::handle};
    return p;
}
} // namespace
TEST_CASE(
    "A04 authorization current context pins scope confirmation and pure effects at every boundary",
    "[authorization][A03][A04]") {
    auto capability = cap();
    CapabilitySnapshot registry;
    registry.capabilities.emplace(capability.identifier, capability);
    ExecutionPlan plan;
    plan.pins.push_back(fingerprint_capability(capability).value());
    AuthorizationGate gate;
    auto c = context();
    auto p = policy();
    AuthorizationRequest request;
    request.capability = capability.identifier;
    request.concrete_input_digest =
        domain_digest("flamoris.operation-input/1\n", JsonValue::Object{}).value();
    request.object_scopes = {"object.a"};
    request.deadline_ms = 90;
    for (auto boundary : {AuthorizationBoundary::admission, AuthorizationBoundary::dispatch,
                          AuthorizationBoundary::retry, AuthorizationBoundary::resume}) {
        request.boundary = boundary;
        REQUIRE(gate.check(c, p, plan, registry, request));
        auto revoked = c;
        revoked.revoked = true;
        REQUIRE_FALSE(gate.check(revoked, p, plan, registry, request));
        auto denied = p;
        denied.capabilities.clear();
        REQUIRE_FALSE(gate.check(c, denied, plan, registry, request));
        denied = p;
        denied.object_scopes.clear();
        REQUIRE_FALSE(gate.check(c, denied, plan, registry, request));
    }
    p.confirmation_effects = 1;
    REQUIRE_FALSE(gate.check(c, p, plan, registry, request));
    c.confirmations.push_back(
        {"local", capability.identifier, request.concrete_input_digest, 1, 50});
    REQUIRE(gate.check(c, p, plan, registry, request));
    request.concrete_input_digest = std::string(64, 'b');
    REQUIRE_FALSE(gate.check(c, p, plan, registry, request));
    request.concrete_input_digest =
        domain_digest("flamoris.operation-input/1\n", JsonValue::Object{}).value();
    request.now_ms = 50;
    REQUIRE_FALSE(gate.check(c, p, plan, registry, request));
    p.confirmation_effects = 0;
    request.now_ms = 90;
    REQUIRE_FALSE(gate.check(c, p, plan, registry, request));
    request.now_ms = 0;
    request.state_accessible = false;
    REQUIRE_FALSE(gate.check(c, p, plan, registry, request));
    request.state_accessible = true;
    plan.pins.clear();
    REQUIRE_FALSE(gate.check(c, p, plan, registry, request));
}
TEST_CASE("A05 finite cumulative reservation is atomic in both competition orders",
          "[authorization][A05]") {
    RunLimits limits;
    limits.max_attempts = 10;
    limits.max_output_bytes = 10;
    for (bool reverse : {false, true}) {
        (void)reverse;
        RunBudget budget(limits);
        BudgetCharge first;
        first.attempts = 6;
        first.output_bytes = 6;
        REQUIRE(budget.reserve(first));
        REQUIRE_FALSE(budget.reserve(first));
        REQUIRE(budget.used().attempts == 6);
        REQUIRE(budget.used().output_bytes == 6);
        BudgetCharge last;
        last.attempts = 4;
        last.output_bytes = 4;
        REQUIRE(budget.reserve(last));
        last.attempts = 1;
        REQUIRE_FALSE(budget.reserve(last));
        REQUIRE(budget.used().attempts == 10);
        REQUIRE(budget.remaining().max_attempts == 0);
        REQUIRE(budget.remaining().max_output_bytes == 0);
    }
}
TEST_CASE("A25 B-RETRY01 unknown writes cannot blindly retry and prior attempts cannot overlap",
          "[authorization][A25][B-RETRY01]") {
    auto c = cap();
    c.retry_permitted = true;
    c.max_attempts = 3;
    RetryEvidence e;
    e.previous_stopped = true;
    e.transient = true;
    e.max_attempts = 3;
    e.completed_attempts = 1;
    e.previous_provider_key = e.next_provider_key = "stable";
    e.outcome = AttemptOutcome::not_dispatched;
    REQUIRE(authorize_retry(c, e, 1, 10));
    e.previous_stopped = false;
    REQUIRE_FALSE(authorize_retry(c, e, 1, 10));
    e.previous_stopped = true;
    e.outcome = AttemptOutcome::unknown;
    auto uncertain = authorize_retry(c, e, 1, 10);
    REQUIRE_FALSE(uncertain);
    REQUIRE(uncertain.error().external_outcome() == ExternalOutcome::unknown);
    REQUIRE(uncertain.error().retry_disposition() == RetryDisposition::reconciliation_required);
    REQUIRE(authorize_retry(c, e, 10, 10).error().external_outcome() == ExternalOutcome::unknown);
    c.provider_deduplication = true;
    e.reconciled = true;
    REQUIRE(authorize_retry(c, e, 1, 10));
    e.next_provider_key = "new";
    REQUIRE_FALSE(authorize_retry(c, e, 1, 10));
    e.next_provider_key = "stable";
    e.outcome = AttemptOutcome::confirmed_success;
    REQUIRE_FALSE(authorize_retry(c, e, 1, 10));
    e.outcome = AttemptOutcome::not_dispatched;
    REQUIRE_FALSE(authorize_retry(c, e, 10, 10));
    e.job_stopping_or_terminal = true;
    REQUIRE_FALSE(authorize_retry(c, e, 1, 10));
}
TEST_CASE("A35 current independent observation scope and handle expiry", "[authorization][A35]") {
    AuthorizationGate gate;
    auto c = context();
    auto p = policy();
    REQUIRE(gate.check_access(c, p, "local", AccessSurface::status, 1));
    REQUIRE_FALSE(gate.check_access(c, p, "local", AccessSurface::trace, 1));
    REQUIRE_FALSE(gate.check_access(c, p, "local", AccessSurface::replay, 1));
    REQUIRE_FALSE(gate.check_access(c, p, "someone", AccessSurface::status, 1));
    REQUIRE(gate.check_access(c, p, "local", AccessSurface::handle, 1, "object.a", 2));
    REQUIRE_FALSE(gate.check_access(c, p, "local", AccessSurface::handle, 2, "object.a", 2));
    c.revoked = true;
    REQUIRE_FALSE(gate.check_access(c, p, "local", AccessSurface::status, 1));
}
TEST_CASE("A27 keyed pending owner shares common decisions and unkeyed never consults index",
          "[submission][A27]") {
    SubmissionIndex index({1, 1});
    auto a = index.claim("local", "workflow", {}, digest, 0),
         b = index.claim("local", "workflow", {}, digest, 0);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a.value().owner);
    REQUIRE(b.value().owner);
    REQUIRE(a.value().run != b.value().run);
    REQUIRE(a.value().pending != b.value().pending);
    REQUIRE(index.keyed_entries() == 0);
    REQUIRE(index.keyed_lookups() == 0);
    REQUIRE(index.reject(a.value(), ErrorEnvelope::make(ErrorCode::permission_denied)));
    REQUIRE(index.admit(b.value(), 1));
    REQUIRE(index.poll(a.value()).value()->rejection.has_value());
    REQUIRE(index.poll(b.value()).value()->run == b.value().run);
    auto owner = index.claim("local", "workflow", "key", digest, 2);
    auto waiter = index.claim("local", "workflow", "key", digest, 2);
    REQUIRE(owner);
    REQUIRE(waiter);
    REQUIRE(owner.value().owner);
    REQUIRE_FALSE(waiter.value().owner);
    REQUIRE(owner.value().pending == waiter.value().pending);
    REQUIRE(owner.value().run == waiter.value().run);
    REQUIRE_FALSE(index.poll(waiter.value()).value());
    REQUIRE_FALSE(index.claim("local", "workflow", "key", std::string(64, 'b'), 2));
    REQUIRE_FALSE(index.admit(waiter.value(), 2));
    REQUIRE(index.reject(owner.value(), ErrorEnvelope::make(ErrorCode::budget_exceeded)));
    auto x = index.poll(owner.value()).value(), y = index.poll(waiter.value()).value();
    REQUIRE(x == y);
    REQUIRE(x->rejection->code() == ErrorCode::budget_exceeded);
    REQUIRE(index.keyed_entries() == 0);
    auto next = index.claim("local", "workflow", "key", digest, 3);
    REQUIRE(next);
    REQUIRE(next.value().owner);
    REQUIRE(next.value().run != owner.value().run);
    REQUIRE_FALSE(index.admit(owner.value(), 3));
    REQUIRE(index.admit(next.value(), 3));
    auto duplicate = index.claim("local", "workflow", "key", digest, 4);
    REQUIRE(duplicate);
    REQUIRE_FALSE(duplicate.value().owner);
    REQUIRE(index.poll(duplicate.value()).value()->run == next.value().run);
}
TEST_CASE("A27 submission retention expiry disconnect bounded waiters and restart",
          "[submission][A27][A33]") {
    SubmissionBounds bounds;
    bounds.retention_ms = 5;
    bounds.max_waiters_per_claim = 1;
    SubmissionIndex index({1, 1}, bounds);
    auto owner = index.claim("local", "inference", "key", digest, 0);
    REQUIRE(owner);
    auto waiter = index.claim("local", "inference", "key", digest, 0);
    REQUIRE(waiter);
    REQUIRE_FALSE(index.claim("local", "inference", "key", digest, 0));
    index.release_receipt(owner.value());
    REQUIRE(index.admit(owner.value(), 1));
    REQUIRE(index.poll(waiter.value()).value()->run == owner.value().run);
    index.expire(6);
    auto next = index.claim("local", "inference", "key", digest, 6);
    REQUIRE(next);
    REQUIRE(next.value().run != owner.value().run);
    SubmissionIndex restarted({2, 2}, bounds);
    auto fresh = restarted.claim("local", "inference", "key", digest, 6);
    REQUIRE(fresh);
    REQUIRE(fresh.value().run != next.value().run);
    REQUIRE_FALSE(restarted.admit(next.value(), 7));
}
TEST_CASE(
    "A04 A35 scope derives from concrete inputs and service handles require registered validation",
    "[authorization][A04][A35]") {
    CapabilitySnapshot registry;
    auto capability = cap();
    ValueSchema target;
    target.kind = ValueSchema::Kind::string;
    target.max_bytes = 64;
    capability.input_schema.properties["target"] = target;
    capability.input_schema.required.insert("target");
    capability.object_scope_fields = {"target"};
    registry.capabilities.emplace(capability.identifier, capability);
    ExecutionPlan plan;
    plan.pins = {fingerprint_capability(capability).value()};
    AuthorizationRequest request;
    request.capability = capability.identifier;
    request.deadline_ms = 90;
    request.concrete_inputs = {{"target", "object.a"}};
    auto c = context();
    auto p = policy();
    AuthorizationGate gate;
    REQUIRE(gate.check(c, p, plan, registry, request));
    request.concrete_inputs["target"] = "object.b";
    request.object_scopes = {"object.a"};
    REQUIRE_FALSE(gate.check(c, p, plan, registry, request));
    request.concrete_inputs["target"] = "object.a";
    ValueSchema handle;
    handle.kind = ValueSchema::Kind::object;
    handle.service_handle_type = "registered.asset/1";
    handle.properties["owner"] = target;
    handle.properties["scope"] = target;
    ValueSchema expiry;
    expiry.kind = ValueSchema::Kind::integer;
    expiry.minimum = 0;
    expiry.maximum = 1000;
    handle.properties["expires_at_ms"] = expiry;
    handle.required = {"owner", "scope", "expires_at_ms"};
    capability.input_schema.properties["asset"] = handle;
    capability.input_schema.required.insert("asset");
    request.concrete_inputs["asset"] =
        JsonValue::Object{{"owner", "local"}, {"scope", "object.a"}, {"expires_at_ms", 50.0}};
    registry.capabilities.at(capability.identifier) = capability;
    plan.pins = {fingerprint_capability(capability).value()};
    REQUIRE_FALSE(gate.check(c, p, plan, registry, request));
    int validations = 0;
    capability.handle_validator_revision = "registered.asset-validator/1";
    capability.handle_validator = [&](const JsonValue &value, std::string_view subject,
                                      std::uint64_t) {
        ++validations;
        if (subject != "local")
            return Result<ValidatedHandleAccess>::failure(
                ErrorEnvelope::make(ErrorCode::permission_denied));
        const auto &object = std::get<JsonValue::Object>(value.data);
        return Result<ValidatedHandleAccess>::success(
            {std::get<std::string>(object.at("owner").data),
             std::get<std::string>(object.at("scope").data),
             static_cast<std::uint64_t>(std::get<double>(object.at("expires_at_ms").data))});
    };
    registry.capabilities.at(capability.identifier) = capability;
    plan.pins = {fingerprint_capability(capability).value()};
    REQUIRE(gate.check(c, p, plan, registry, request));
    REQUIRE(validations == 1);
    request.now_ms = 50;
    REQUIRE_FALSE(gate.check(c, p, plan, registry, request));
    request.now_ms = 0;
    std::get<JsonValue::Object>(request.concrete_inputs["asset"].data)["scope"] = "object.b";
    REQUIRE_FALSE(gate.check(c, p, plan, registry, request));
}
TEST_CASE(
    "A04 plan admission checks every potential capability without fabricating dependent inputs",
    "[authorization][A04]") {
    auto capability = cap();
    CapabilitySnapshot registry;
    registry.capabilities.emplace(capability.identifier, capability);
    ExecutionPlan plan;
    plan.pins = {fingerprint_capability(capability).value()};
    auto c = context();
    auto p = policy();
    AuthorizationGate gate;
    REQUIRE(gate.authorize_plan_admission(c, p, plan, registry, 0));
    p.permitted_effects = 0;
    REQUIRE_FALSE(gate.authorize_plan_admission(c, p, plan, registry, 0));
    p = policy();
    c.capabilities.clear();
    REQUIRE_FALSE(gate.authorize_plan_admission(c, p, plan, registry, 0));
    c = context();
    REQUIRE_FALSE(gate.authorize_plan_admission(c, p, plan, registry, 100));
    registry.capabilities.at(capability.identifier).available = false;
    REQUIRE_FALSE(gate.authorize_plan_admission(c, p, plan, registry, 0));
}

TEST_CASE("A35 returned handles recheck current scope expiry and union alternatives",
          "[authorization][A04][A35]") {
    auto capability = cap();
    ValueSchema text;
    text.kind = ValueSchema::Kind::string;
    text.max_bytes = 64;
    auto handle = text;
    handle.service_handle_type = "registered.asset/1";
    capability.output_schema.kind = ValueSchema::Kind::union_value;
    capability.output_schema.variants = {text, handle};
    int validations = 0;
    ValidatedHandleAccess access{"local", "object.a", 50};
    capability.handle_validator_revision = "registered.asset/1";
    capability.handle_validator = [&](const JsonValue &value, std::string_view, std::uint64_t) {
        ++validations;
        if (std::get<std::string>(value.data) != "opaque-service-handle")
            return Result<ValidatedHandleAccess>::failure(
                ErrorEnvelope::make(ErrorCode::permission_denied));
        return Result<ValidatedHandleAccess>::success(access);
    };
    CapabilitySnapshot registry;
    registry.capabilities.emplace(capability.identifier, capability);
    auto pin = fingerprint_capability(capability).value();
    ExecutionPlan plan;
    plan.pins = {pin};
    AuthorizationGate gate;
    auto c = context();
    auto p = policy();
    JsonValue value("opaque-service-handle");
    REQUIRE(gate.validate_result_handles(c, p, plan, registry, capability.identifier, value, 1));
    REQUIRE(validations == 1);
    REQUIRE(gate.check_retained_result_handles(c, p, pin, capability.output_schema, registry, value,
                                               2));
    access.object_scope = "object.b";
    REQUIRE_FALSE(
        gate.validate_result_handles(c, p, plan, registry, capability.identifier, value, 3));
    access.object_scope = "object.a";
    REQUIRE_FALSE(gate.check_retained_result_handles(c, p, pin, capability.output_schema, registry,
                                                     value, 50));
    c.access.erase(AccessSurface::handle);
    REQUIRE_FALSE(
        gate.validate_result_handles(c, p, plan, registry, capability.identifier, value, 3));
    c = context();
    access.owner = "another-subject";
    REQUIRE_FALSE(
        gate.validate_result_handles(c, p, plan, registry, capability.identifier, value, 3));
    access.owner = "local";
    registry.capabilities.at(capability.identifier).available = false;
    REQUIRE_FALSE(gate.check_retained_result_handles(c, p, pin, capability.output_schema, registry,
                                                     value, 3));
    // Historical immutable text still follows result-access authorization, independent of execution
    // pins.
    REQUIRE(gate.check_retained_result_handles(c, p, pin, text, registry, "historic text", 3));
    registry.capabilities.at(capability.identifier).available = true;
    registry.capabilities.at(capability.identifier).handle_validator_revision = "changed/2";
    REQUIRE_FALSE(gate.check_retained_result_handles(c, p, pin, capability.output_schema, registry,
                                                     value, 3));
    REQUIRE(gate.check_retained_result_handles(c, p, pin, text, registry, "historic text", 3));
    registry.capabilities.clear();
    REQUIRE(gate.check_retained_result_handles(c, p, pin, text, registry, "historic text", 3));
}
