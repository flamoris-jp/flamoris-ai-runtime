#include "catch_amalgamated.hpp"
#include "flamoris/runtime/registered_adapter.hpp"
#include <stdexcept>
using namespace flamoris::runtime;
namespace {
CapabilityContract contract() {
    CapabilityContract c(EffectSet::from_names({"write", "external"}).value());
    c.identifier = "service.write";
    c.version = "1";
    c.adapter_revision = "registered/1";
    c.input_schema.kind = ValueSchema::Kind::object;
    ValueSchema text;
    text.kind = ValueSchema::Kind::string;
    text.max_bytes = 32;
    c.input_schema.properties.emplace("text", text);
    c.input_schema.required.insert("text");
    c.output_schema = text;
    c.max_output_bytes = 64;
    return c;
}
struct FaultingProvider final : RegisteredProviderPort {
    enum class Fault { none, accepted_response_lost, oversized, wrong_schema, nested_secret };
    Fault fault{Fault::none};
    std::size_t calls{0}, queries{0}, largest_buffer{0};
    ExternalOutcome invoke(std::string_view, const JsonValue &, std::uint64_t,
                           BoundedProviderSink &sink) override {
        ++calls;
        if (fault == Fault::accepted_response_lost)
            throw std::runtime_error("secret token and private endpoint");
        sink.record_outcome(ExternalOutcome::confirmed_success);
        if (fault == Fault::oversized) {
            REQUIRE(sink.append(std::string(32, 'a')));
            REQUIRE_FALSE(sink.append(std::string(33, 'b')));
            REQUIRE_FALSE(sink.append("ignored"));
        } else if (fault == Fault::wrong_schema)
            (void)sink.append("{\"wrong\":true}");
        else if (fault == Fault::nested_secret)
            (void)sink.append("{\"causes\":[{\"secret\":\"credential\"}]}");
        else
            (void)sink.append("\"accepted\"");
        largest_buffer = std::max(largest_buffer, sink.bytes().size());
        return ExternalOutcome::confirmed_success;
    }
    ExternalOutcome reconcile(std::string_view, std::uint64_t) override {
        ++queries;
        return ExternalOutcome::unknown;
    }
};
struct Fixture {
    CapabilityContract capability{contract()};
    CapabilitySnapshot registry;
    ExecutionPlan plan;
    CapabilityPin pin;
    JsonValue input{JsonValue::Object{{"text", "hello"}}};
    AuthorizationContext context;
    PolicySnapshot policy;
    AuthorizationRequest request;
    DispatchTicket ticket{JobId{RunId{RuntimeInstanceId{41, 1}, 1}, 1}, 1, 1};
    AdapterAuthority authority;
    Fixture() {
        registry.capabilities.emplace(capability.identifier, capability);
        pin = fingerprint_capability(capability).value();
        plan.pins.push_back(pin);
        context.subject = "local";
        context.expires_at_ms = 100;
        context.permitted_effects = 63;
        context.capabilities.insert(capability.identifier);
        policy.capabilities = context.capabilities;
        policy.permitted_effects = 63;
        request.capability = capability.identifier;
        request.deadline_ms = 90;
        request.concrete_inputs = std::get<JsonValue::Object>(input.data);
        request.concrete_input_digest =
            domain_digest("flamoris.operation-input/1\n", input).value();
    }
    AdapterGrant grant(AdapterPurpose purpose = AdapterPurpose::invoke) {
        auto granted = authority.authorize(context, policy, plan, registry, request, ticket, pin,
                                           input, "operation-1", purpose);
        REQUIRE(granted);
        return std::move(granted).value();
    }
};
} // namespace
TEST_CASE("A25 registered write response loss is unknown with separate bounded reconciliation and "
          "no redispatch") {
    Fixture f;
    FaultingProvider provider;
    provider.fault = FaultingProvider::Fault::accepted_response_lost;
    RegisteredCapabilityAdapter adapter(f.capability, provider, 2, 1);
    auto result = adapter.invoke({f.grant(), f.input, 64}, 1);
    REQUIRE(result.handoff_attempted);
    REQUIRE(result.external_outcome == ExternalOutcome::unknown);
    REQUIRE(result.error->code() == ErrorCode::outcome_unknown);
    REQUIRE(result.error->retry_disposition() == RetryDisposition::reconciliation_required);
    REQUIRE(result.error->message().find("secret") == std::string_view::npos);
    REQUIRE_FALSE(result.value);
    auto duplicate = adapter.invoke({f.grant(), f.input, 64}, 2);
    REQUIRE_FALSE(duplicate.handoff_attempted);
    REQUIRE(provider.calls == 1);
    f.ticket.attempt = 2;
    f.ticket.dispatch_generation = 2;
    REQUIRE_FALSE(adapter.invoke({f.grant(), f.input, 64}, 2).handoff_attempted);
    REQUIRE(provider.calls == 1); // A fresh ticket cannot blindly repeat one semantic operation.
    f.ticket.attempt = 1;
    f.ticket.dispatch_generation = 1;
    auto reconciled = adapter.reconcile(f.grant(AdapterPurpose::reconcile), 2);
    REQUIRE(reconciled.external_outcome == ExternalOutcome::unknown);
    REQUIRE(provider.calls == 1);
    REQUIRE(provider.queries == 1);
    auto repeat = adapter.reconcile(f.grant(AdapterPurpose::reconcile), 3);
    REQUIRE_FALSE(repeat.handoff_attempted);
    REQUIRE(provider.queries == 1);
}
TEST_CASE("A26 streamed oversized or invalid confirmed write output never binds or retries") {
    for (const auto fault :
         {FaultingProvider::Fault::oversized, FaultingProvider::Fault::wrong_schema,
          FaultingProvider::Fault::nested_secret}) {
        Fixture f;
        FaultingProvider provider;
        provider.fault = fault;
        RegisteredCapabilityAdapter adapter(f.capability, provider);
        auto result = adapter.invoke({f.grant(), f.input, 64}, 1);
        REQUIRE(result.external_outcome == ExternalOutcome::confirmed_success);
        REQUIRE_FALSE(result.value);
        REQUIRE(result.error);
        REQUIRE(result.error->code() == (fault == FaultingProvider::Fault::oversized
                                             ? ErrorCode::result_too_large
                                             : ErrorCode::invalid_result));
        REQUIRE(result.error->external_outcome() == ExternalOutcome::confirmed_success);
        REQUIRE(result.error->cause_codes().empty());
        REQUIRE(provider.calls == 1);
        REQUIRE(provider.largest_buffer <= 64);
    }
}
TEST_CASE("A04 registered adapter grant is current input-bound single-use and deadline-bound") {
    Fixture f;
    FaultingProvider provider;
    RegisteredCapabilityAdapter adapter(f.capability, provider);
    f.context.revoked = true;
    REQUIRE_FALSE(f.authority.authorize(f.context, f.policy, f.plan, f.registry, f.request,
                                        f.ticket, f.pin, f.input, "operation-1"));
    f.context.revoked = false;
    auto changed_input = JsonValue::Object{{"text", "modified"}};
    auto rejected = adapter.invoke({f.grant(), changed_input, 64}, 1);
    REQUIRE_FALSE(rejected.handoff_attempted);
    REQUIRE(provider.calls == 0);
    rejected = adapter.invoke({f.grant(), f.input, 64}, 90);
    REQUIRE_FALSE(rejected.handoff_attempted);
    REQUIRE(provider.calls == 0);
    f.context.expires_at_ms = 20;
    rejected = adapter.invoke({f.grant(), f.input, 64}, 20);
    REQUIRE_FALSE(rejected.handoff_attempted);
    REQUIRE(provider.calls == 0);
    f.context.expires_at_ms = 100;
    auto accepted = adapter.invoke({f.grant(), f.input, 64}, 89);
    REQUIRE(accepted.value == JsonValue("accepted"));
    REQUIRE(provider.calls == 1);
    REQUIRE_FALSE(adapter.invoke({f.grant(), f.input, 64}, 89).handoff_attempted);
}
TEST_CASE("C11 registered identity implementation returns schema validated bounded data") {
    Fixture f;
    f.capability.effects = EffectSet::from_names({"pure"}).value();
    f.capability.output_schema = f.capability.input_schema;
    f.registry.capabilities.insert_or_assign(f.capability.identifier, f.capability);
    f.pin = fingerprint_capability(f.capability).value();
    f.plan.pins = {f.pin};
    IdentityProvider provider;
    RegisteredCapabilityAdapter adapter(f.capability, provider);
    auto result = adapter.invoke({f.grant(), f.input, 64}, 1);
    REQUIRE(result.value == f.input);
    REQUIRE(result.external_outcome == ExternalOutcome::confirmed_success);
}
