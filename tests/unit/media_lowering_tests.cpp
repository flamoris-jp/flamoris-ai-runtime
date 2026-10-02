#include "catch_amalgamated.hpp"
#include "flamoris/runtime/media_lowering.hpp"
#include <algorithm>
using namespace flamoris::runtime;
namespace {
std::string digest(char c = 'a') { return "sha256:" + std::string(64, c); }
ValueSchema text() {
    ValueSchema schema;
    schema.kind = ValueSchema::Kind::string;
    schema.max_bytes = 128;
    return schema;
}
Binding literal(JsonValue value) { return {std::move(value), {}}; }
Binding ref(Reference::Source source, std::string name) {
    return {{}, Reference{source, std::move(name), {}}};
}
struct Fixture {
    CapabilitySnapshot registry;
    std::map<std::string, MediaCapabilityRegistration> approved;
    MediaLoweringRequest request;
    Fixture() {
        request.identity = {digest(),          digest('b'), digest('c'),
                            digest('d'),       digest('e'), "generation.compiler/2",
                            "media.lowering/1"};
        request.effects = EffectSet::from_names({"external"}).value();
        for (const auto *name : {"generation.stage_a", "generation.stage_b"}) {
            CapabilityContract c(request.effects);
            c.identifier = name;
            c.version = "1";
            c.adapter_revision = "fixture/1";
            c.resource_contract_digest = std::string(64, 'f');
            c.input_schema.kind = ValueSchema::Kind::object;
            c.input_schema.properties["text"] = text();
            c.input_schema.required.insert("text");
            c.output_schema = text();
            registry.capabilities.emplace(name, c);
            approved.emplace(name, MediaCapabilityRegistration{fingerprint_capability(c).value()});
        }
        request.input_schemas["prompt"] = text();
        request.input_values["prompt"] = "hello";
        request.operations = {{"root/first",
                               approved.at("generation.stage_a").pin,
                               {{"text", ref(Reference::Source::input, "prompt")}},
                               {},
                               1000},
                              {"root/second",
                               approved.at("generation.stage_b").pin,
                               {{"text", ref(Reference::Source::node, "root/first")}},
                               {},
                               1000}};
        request.outputs["result"] = ref(Reference::Source::node, "root/second");
    }
    Result<LoweredMediaSubmission> lower() {
        return lower_media_submission(request, registry, approved);
    }
};
void code(const Result<LoweredMediaSubmission> &result, ErrorCode expected) {
    REQUIRE_FALSE(result);
    REQUIRE(result.error().code() == expected);
}
} // namespace
TEST_CASE("B01 media lowering preserves independent Runtime and invocation identities", "[media]") {
    Fixture f;
    auto first = f.lower();
    REQUIRE(first);
    REQUIRE(first.value().submission.plan->static_jobs == 3);
    REQUIRE(first.value().submission.plan->static_attempts == 2);
    REQUIRE(first.value().submission.plan->effects == f.request.effects);
    REQUIRE_FALSE(first.value().submission.idempotency_key);
    REQUIRE(first.value().occurrence_nodes.at("root/first") !=
            first.value().occurrence_nodes.at("root/second"));
    std::reverse(f.request.operations.begin(), f.request.operations.end());
    auto reordered = f.lower();
    REQUIRE(reordered);
    REQUIRE(first.value().canonical_submission == reordered.value().canonical_submission);
    REQUIRE(first.value().relation_digest == reordered.value().relation_digest);
    f.request.input_values["prompt"] = "changed";
    f.request.identity.invocation_digest = digest('f');
    auto changed = f.lower();
    REQUIRE(changed);
    REQUIRE(first.value().submission.plan->fingerprint ==
            changed.value().submission.plan->fingerprint);
    REQUIRE(first.value().relation_digest != changed.value().relation_digest);
    REQUIRE(first.value().identity.structural_digest == changed.value().identity.structural_digest);
    REQUIRE(first.value().submission.plan->steps.front().dependencies.empty());
    REQUIRE(changed.value().submission.plan->steps.back().dependencies.size() == 1);
}
TEST_CASE("B02 exact approved media contracts reject stale unavailable and unregistered targets",
          "[media]") {
    Fixture f;
    f.registry.capabilities.at("generation.stage_a").adapter_revision = "fixture/2";
    code(f.lower(), ErrorCode::plan_stale);
    f = Fixture{};
    f.registry.capabilities.at("generation.stage_a").available = false;
    code(f.lower(), ErrorCode::capability_unavailable);
    f = Fixture{};
    f.approved.erase("generation.stage_a");
    code(f.lower(), ErrorCode::permission_denied);
    f = Fixture{};
    f.request.operations[0].pin.fingerprint = std::string(64, '0');
    code(f.lower(), ErrorCode::plan_stale);
    f = Fixture{};
    f.registry.capabilities.erase("generation.stage_a");
    code(f.lower(), ErrorCode::unknown_capability);
}
TEST_CASE("B04 lowering cannot turn public root admission into an internal operation", "[media]") {
    Fixture f;
    f.approved.at("generation.stage_a").purpose = MediaOperationPurpose::public_root_admission;
    code(f.lower(), ErrorCode::permission_denied);
    f = Fixture{};
    auto &c = f.registry.capabilities.at("generation.stage_a");
    c.resource_contract_digest.clear();
    f.approved.at(c.identifier).pin = fingerprint_capability(c).value();
    f.request.operations[0].pin = f.approved.at(c.identifier).pin;
    code(f.lower(), ErrorCode::invalid_workflow);
}
TEST_CASE("B06 omitted effects and unordered overlapping writes fail compilation", "[media]") {
    Fixture f;
    f.request.effects = EffectSet::from_names({"pure"}).value();
    code(f.lower(), ErrorCode::invalid_workflow);
    f = Fixture{};
    f.request.effects = EffectSet::from_names({"external", "write"}).value();
    for (auto &[name, c] : f.registry.capabilities) {
        c.effects = f.request.effects;
        f.approved.at(name).pin = fingerprint_capability(c).value();
    }
    for (auto &op : f.request.operations) {
        op.pin = f.approved.at(op.pin.identifier).pin;
        op.inputs["text"] = ref(Reference::Source::input, "prompt");
    }
    f.request.outputs["first"] = ref(Reference::Source::node, "root/first");
    REQUIRE_FALSE(f.lower());
    f.request.operations[1].dependencies.push_back("root/first");
    REQUIRE(f.lower());
}
TEST_CASE("B07 media ceilings include root Job events output and finite provider deadlines",
          "[media]") {
    Fixture f;
    f.request.limits.max_jobs = 2;
    code(f.lower(), ErrorCode::budget_exceeded);
    f = Fixture{};
    f.request.limits.max_attempts = 1;
    code(f.lower(), ErrorCode::budget_exceeded);
    f = Fixture{};
    f.request.limits.max_event_bytes = 1;
    code(f.lower(), ErrorCode::budget_exceeded);
    f = Fixture{};
    f.request.limits.timeout_ms = 60001;
    code(f.lower(), ErrorCode::budget_exceeded);
    f = Fixture{};
    f.request.operations[0].timeout_ms = 60001;
    code(f.lower(), ErrorCode::budget_exceeded);
    f = Fixture{};
    f.request.limits.max_output_bytes = 1048577;
    code(f.lower(), ErrorCode::budget_exceeded);
    f = Fixture{};
    f.request.limits.max_trace_bytes = 9007199254740992ULL;
    code(f.lower(), ErrorCode::budget_exceeded);
}
TEST_CASE("B07 lowering rejects unsafe binary64 integers instead of rounding", "[media]") {
    Fixture f;
    auto integer = text();
    integer.kind = ValueSchema::Kind::integer;
    integer.minimum = 0;
    integer.maximum = 9007199254740992.0;
    f.request.input_schemas["prompt"] = integer;
    f.request.input_values["prompt"] = 9007199254740992.0;
    REQUIRE_FALSE(f.lower());
    f = Fixture{};
    f.request.operations[1].inputs["text"].reference->path.push_back(9007199254740992ULL);
    code(f.lower(), ErrorCode::invalid_reference);
}
TEST_CASE("B03 malformed bindings domains cycles and hidden occurrences fail closed", "[media]") {
    Fixture f;
    f.request.operations[1].inputs["text"] = literal(true);
    REQUIRE_FALSE(f.lower());
    f = Fixture{};
    f.request.operations[1].inputs["text"].literal = "ambiguous";
    code(f.lower(), ErrorCode::invalid_reference);
    f = Fixture{};
    f.request.operations[0].inputs["text"] = ref(Reference::Source::node, "root/second");
    REQUIRE_FALSE(f.lower());
    f = Fixture{};
    f.request.operations[1].inputs["text"] = literal("independent");
    code(f.lower(), ErrorCode::invalid_workflow);
    f = Fixture{};
    f.request.operations[1].occurrence = "root/first";
    code(f.lower(), ErrorCode::invalid_workflow);
    f = Fixture{};
    f.request.identity.root_digest = "file:///private";
    code(f.lower(), ErrorCode::invalid_workflow);
    f = Fixture{};
    f.request.outputs["result"] = literal("result");
    code(f.lower(), ErrorCode::invalid_reference);
}
TEST_CASE("B03 registered immutable handles retain trusted validator provenance without I/O",
          "[media]") {
    Fixture f;
    auto handle = text();
    handle.service_handle_type = "generation.input/1";
    auto &c = f.registry.capabilities.at("generation.stage_a");
    c.input_schema.properties["text"] = handle;
    c.output_schema = handle;
    c.handle_validator_revision = "fixture.handle/1";
    int validations = 0;
    c.handle_validator = [&](const JsonValue &, std::string_view, std::uint64_t) {
        ++validations;
        return Result<ValidatedHandleAccess>::success({"fixture-owner", "fixture-object", 1000});
    };
    auto &second = f.registry.capabilities.at("generation.stage_b");
    second.input_schema.properties["text"] = handle;
    second.output_schema = handle;
    second.handle_validator_revision = c.handle_validator_revision;
    second.handle_validator = c.handle_validator;
    for (auto &op : f.request.operations) {
        f.approved.at(op.pin.identifier).pin =
            fingerprint_capability(f.registry.capabilities.at(op.pin.identifier)).value();
        op.pin = f.approved.at(op.pin.identifier).pin;
    }
    auto lowered = f.lower();
    REQUIRE(lowered);
    REQUIRE(validations == 0);
    REQUIRE(lowered.value().submission.plan->inputs.at("prompt").service_handle_type ==
            "generation.input/1");
    REQUIRE(lowered.value().submission.plan->result_outputs.at("result").handle_validator ==
            f.request.operations[1].pin);
    f.request.operations[0].inputs["text"] = literal("forged-handle");
    REQUIRE_FALSE(f.lower());
    REQUIRE(validations == 0);
}

TEST_CASE("media lowering permits explicit ordering that also carries data", "[media]") {
    Fixture f;
    f.request.operations[1].dependencies.push_back("root/first");
    REQUIRE(f.lower());
    f.request.operations[1].dependencies.push_back("root/first");
    code(f.lower(), ErrorCode::invalid_reference);
}
