#include "catch_amalgamated.hpp"
#include "flamoris/runtime/compiler.hpp"
#include <bit>
using namespace flamoris::runtime;
namespace {
ValueSchema text_schema(std::uint64_t bound = 128) {
    ValueSchema s;
    s.kind = ValueSchema::Kind::string;
    s.max_bytes = bound;
    return s;
}
CapabilityContract capability(std::string id, std::uint8_t effects = 1) {
    CapabilityContract c(EffectSet::from_mask(effects).value());
    c.identifier = std::move(id);
    c.version = "1";
    c.adapter_revision = "configured/1";
    c.input_schema.kind = ValueSchema::Kind::object;
    c.input_schema.properties["text"] = text_schema();
    c.input_schema.required.insert("text");
    c.output_schema = text_schema();
    return c;
}
CapabilitySnapshot registry() {
    CapabilitySnapshot r;
    r.capabilities.emplace("algorithm.echo", capability("algorithm.echo"));
    auto inference = capability("model.native");
    inference.inference = true;
    inference.native_pins = {{"model", "tiny/1"},
                             {"processor", "utf8/1"},
                             {"tokenizer", "bytes/1"},
                             {"execution_profile", "native/1"},
                             {"compute", "cpu/1"}};
    r.capabilities.emplace(inference.identifier, std::move(inference));
    return r;
}
JsonValue literal(std::string s) { return JsonValue::Object{{"literal", std::move(s)}}; }
JsonValue reference(std::string source, std::string name) {
    return JsonValue::Object{{"ref", JsonValue::Object{{"source", std::move(source)},
                                                       {"name", std::move(name)},
                                                       {"path", JsonValue::Array{}}}}};
}
JsonValue node(std::string id, std::string type = "algorithm.echo",
               JsonValue binding = literal("hello")) {
    return JsonValue::Object{{"id", std::move(id)},
                             {"type", std::move(type)},
                             {"with", JsonValue::Object{{"text", std::move(binding)}}}};
}
JsonValue request(JsonValue::Array nodes = {node("a")}) {
    return JsonValue::Object{
        {"schema_version", "flamoris.submit/1"},
        {"kind", "workflow"},
        {"workflow",
         JsonValue::Object{{"schema_version", "flamoris.workflow/0.1"},
                           {"workflow", JsonValue::Object{{"id", "example"}}},
                           {"inputs", JsonValue::Object{}},
                           {"nodes", std::move(nodes)},
                           {"edges", JsonValue::Array{}},
                           {"outputs", JsonValue::Object{{"result", reference("node", "a")}}},
                           {"limits", JsonValue::Object{}}}},
        {"input_values", JsonValue::Object{}}};
}
JsonValue::Object &obj(JsonValue &v) { return std::get<JsonValue::Object>(v.data); }
JsonValue::Object &workflow(JsonValue &v) { return obj(obj(v).at("workflow")); }
std::string json(const JsonValue &v) {
    auto r = canonical_json(v);
    REQUIRE(r);
    return r.value();
}
} // namespace
TEST_CASE("B-SER01 SAX rejects duplicate decoded keys invalid Unicode bounds and number loss",
          "[compiler][B-SER01]") {
    for (const auto *input :
         {"{\"x\":1,\"\\u0078\":2}", "{\"x\":NaN}", "{\"x\":1e999}", "{\"x\":1e-999}",
          "{\"x\":9007199254740992}", "{\"x\":-9007199254740992}", "{\"x\":\"\\ud800\"}",
          "{\"x\":0} trailing", "{/*comment*/\"x\":0}", "[]"})
        REQUIRE_FALSE(parse_bounded_json(input));
    REQUIRE_FALSE(parse_bounded_json(std::string("{\"x\":\"") + char(0xff) + "\"}"));
    REQUIRE_FALSE(parse_bounded_json("\xef\xbb\xbf{}"));
    REQUIRE(parse_bounded_json("{\"x\":9007199254740991,\"y\":-0,\"z\":5e-324}"));
    JsonBounds b;
    b.max_bytes = 2;
    REQUIRE(parse_bounded_json("{}", b));
    REQUIRE_FALSE(parse_bounded_json("{ }", b));
    b = {};
    b.max_string_bytes = 3;
    REQUIRE(parse_bounded_json("{\"s\":\"abc\"}", b));
    REQUIRE_FALSE(parse_bounded_json("{\"s\":\"abcd\"}", b));
    b = {};
    b.max_depth = 2;
    REQUIRE(parse_bounded_json("{\"x\":[]}", b));
    REQUIRE_FALSE(parse_bounded_json("{\"x\":[[]]}", b));
}
TEST_CASE("B-SER01 canonical JSON uses RFC8785 number string and UTF16 order",
          "[compiler][B-SER01]") {
    auto v = parse_bounded_json("{\"numbers\":[333333333.33333329,1E-27,4.50,2e-3,0.000001,1e-7,-0]"
                                ",\"text\":\"\\u000f\\n\\\\\\\"\"}");
    REQUIRE(v);
    REQUIRE(json(v.value()) == "{\"numbers\":[333333333.3333333,1e-27,4.5,0.002,0.000001,1e-7,0],"
                               "\"text\":\"\\u000f\\n\\\\\\\"\"}");
    JsonValue value = JsonValue::Object{{"\xee\x80\x80", 1.0}, {"\xf0\x90\x80\x80", 2.0}};
    REQUIRE(json(value) == "{\"\xf0\x90\x80\x80\":2,\"\xee\x80\x80\":1}");
    REQUIRE(domain_digest("domain-a\n", value).value() !=
            domain_digest("domain-b\n", value).value());
    REQUIRE(domain_digest("", JsonValue::Object{}).value() ==
            "44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a");
}
TEST_CASE("A01 A02 compiler determinism and topological references", "[compiler][A01][A02]") {
    auto r = registry();
    Compiler c;
    auto input = request({node("z"), node("a", "algorithm.echo", reference("node", "z"))});
    auto first = c.compile_submission(json(input), r);
    REQUIRE(first);
    auto second = c.compile_submission(json(input), r);
    REQUIRE(second);
    REQUIRE(first.value().plan->fingerprint == second.value().plan->fingerprint);
    REQUIRE(first.value().plan->steps[0].id == "z");
    REQUIRE(first.value().plan->steps[1].dependencies == std::vector<std::string>{"z"});
    obj(workflow(input).at("workflow"))["name"] = "display change";
    REQUIRE(c.compile_submission(json(input), r).value().plan->fingerprint ==
            first.value().plan->fingerprint);
    REQUIRE(validate_submission_identity(json(input)).value().request_digest ==
            first.value().request_digest);
    auto bad = request({node("a", "algorithm.echo", reference("node", "b")),
                        node("b", "algorithm.echo", reference("node", "a"))});
    REQUIRE_FALSE(c.compile_submission(json(bad), r));
    bad = request();
    obj(std::get<JsonValue::Array>(workflow(bad).at("nodes").data)[0])["with"] =
        JsonValue::Object{};
    REQUIRE_FALSE(c.compile_submission(json(bad), r));
    bad = request();
    workflow(bad)["unknown"] = true;
    REQUIRE_FALSE(c.compile_submission(json(bad), r));
    bad = request();
    workflow(bad)["schema_version"] = "flamoris.workflow/999";
    REQUIRE_FALSE(c.compile_submission(json(bad), r));
    bad = request({node("a", "https://arbitrary.invalid")});
    REQUIRE_FALSE(c.compile_submission(json(bad), r));
}
TEST_CASE(
    "A03 pin includes schema adapter effect control and native identity excludes availability",
    "[compiler][A03]") {
    auto r = registry();
    auto compiled = Compiler{}.compile_submission(json(request()), r);
    REQUIRE(compiled);
    const auto &plan = *compiled.value().plan;
    r.capabilities.emplace("unrelated", capability("unrelated"));
    REQUIRE(verify_plan_pins(plan, r));
    r.capabilities.at("algorithm.echo").available = false;
    REQUIRE(verify_plan_pins(plan, r, false));
    REQUIRE_FALSE(verify_plan_pins(plan, r));
    r.capabilities.at("algorithm.echo").available = true;
    for (int variant = 0; variant < 4; ++variant) {
        auto changed = r;
        auto &cap = changed.capabilities.at("algorithm.echo");
        if (variant == 0)
            cap.adapter_revision = "changed";
        if (variant == 1)
            cap.output_schema.max_bytes = 12;
        if (variant == 2)
            cap.effects = EffectSet::from_mask(2).value();
        if (variant == 3)
            cap.pausable = true;
        auto result = verify_plan_pins(plan, changed);
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code() == ErrorCode::plan_stale);
    }
}
TEST_CASE("A36 unordered conflicting effects need dependency or trusted disjoint proof",
          "[compiler][A36]") {
    auto r = registry();
    r.capabilities.emplace("write.a", capability("write.a", 4));
    r.capabilities.emplace("write.b", capability("write.b", 4));
    auto input = request({node("a", "write.a"), node("b", "write.b")});
    Compiler c;
    REQUIRE_FALSE(c.compile_submission(json(input), r));
    workflow(input)["edges"] = JsonValue::Array{JsonValue::Object{{"from", "a"}, {"to", "b"}}};
    REQUIRE(c.compile_submission(json(input), r));
    workflow(input)["edges"] = JsonValue::Array{};
    r.capabilities.at("write.a").disjoint_scope = "one";
    r.capabilities.at("write.b").disjoint_scope = "two";
    REQUIRE(c.compile_submission(json(input), r));
    r.capabilities.at("write.b").disjoint_scope = "one";
    REQUIRE_FALSE(c.compile_submission(json(input), r));
}
TEST_CASE("A24 group race rejects write and excessive aggregate attempts", "[compiler][A24]") {
    auto r = registry();
    r.capabilities.emplace("write.a", capability("write.a", 4));
    JsonValue race = JsonValue::Object{
        {"id", "a"},
        {"type", "control.race"},
        {"with", JsonValue::Object{}},
        {"control", JsonValue::Object{{"members", JsonValue::Array{node("x"), node("y")}},
                                      {"accept", JsonValue::Object{{"op", "always"}}},
                                      {"loser_policy", "cancel_unfinished"},
                                      {"timeout_ms", 100.0}}}};
    auto input = request({race});
    REQUIRE(Compiler{}.compile_submission(json(input), r));
    obj(obj(std::get<JsonValue::Array>(workflow(input).at("nodes").data)[0])
            .at("control"))["members"] = JsonValue::Array{node("x", "write.a")};
    REQUIRE_FALSE(Compiler{}.compile_submission(json(input), r));
    input = request({race});
    workflow(input)["limits"] = JsonValue::Object{{"max_attempts", 1.0}};
    REQUIRE_FALSE(Compiler{}.compile_submission(json(input), r));
}
TEST_CASE("A39 direct inference shares normalized pinned plan and bounded inputs",
          "[compiler][A39]") {
    auto r = registry();
    JsonValue direct = JsonValue::Object{
        {"schema_version", "flamoris.submit/1"},
        {"kind", "inference"},
        {"inference", JsonValue::Object{{"type", "model.native"},
                                        {"with", JsonValue::Object{{"text", "hello"}}},
                                        {"limits", JsonValue::Object{}}}}};
    auto explicit_request =
        request({node("inference", "model.native", reference("input", "text"))});
    workflow(explicit_request)["inputs"] =
        JsonValue::Object{{"text", schema_export(text_schema())}};
    workflow(explicit_request)["outputs"] =
        JsonValue::Object{{"result", reference("node", "inference")}};
    obj(explicit_request)["input_values"] = JsonValue::Object{{"text", "hello"}};
    auto a = Compiler{}.compile_submission(json(direct), r),
         b = Compiler{}.compile_submission(json(explicit_request), r);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a.value().plan->single_root_inference);
    REQUIRE(a.value().plan->fingerprint == b.value().plan->fingerprint);
    REQUIRE(a.value().plan->static_jobs == 1);
    REQUIRE_FALSE(a.value().plan->steps[0].child_envelope);
    obj(obj(direct).at("inference"))["with"] = JsonValue::Object{{"text", std::string(129, 'x')}};
    REQUIRE_FALSE(Compiler{}.compile_submission(json(direct), r));
}
TEST_CASE("A27 structurally validated request digest preserves input limits and explicit key rules",
          "[compiler][A27]") {
    auto input = request();
    auto original = validate_submission_identity(json(input));
    REQUIRE(original);
    for (JsonValue key :
         {JsonValue(nullptr), JsonValue(""), JsonValue(1.0), JsonValue(std::string(257, 'x'))}) {
        obj(input)["idempotency_key"] = key;
        REQUIRE_FALSE(validate_submission_identity(json(input)));
    }
    obj(input)["idempotency_key"] = std::string(256, 'x');
    auto keyed = validate_submission_identity(json(input));
    REQUIRE(keyed);
    REQUIRE(keyed.value().request_digest == original.value().request_digest);
    workflow(input)["limits"] = JsonValue::Object{{"max_attempts", 128.0}};
    REQUIRE(validate_submission_identity(json(input)).value().request_digest !=
            original.value().request_digest);
}
TEST_CASE("A07 child compilation intersects exact pins remaining limits and depth",
          "[compiler][A07]") {
    auto r = registry();
    ChildEnvelope envelope;
    envelope.identifier = "children";
    envelope.revision = "1";
    envelope.capabilities = {fingerprint_capability(r.capabilities.at("algorithm.echo")).value()};
    envelope.max_children = 2;
    envelope.max_attempts = 1;
    envelope.max_depth = 1;
    envelope.max_output_bytes = 1024;
    RunLimits remaining;
    auto input = request();
    REQUIRE(compile_child_fragment(json(input), r, envelope, remaining, 1));
    REQUIRE_FALSE(compile_child_fragment(json(input), r, envelope, remaining, 2));
    auto small = remaining;
    small.max_jobs = 1;
    REQUIRE_FALSE(compile_child_fragment(json(input), r, envelope, small, 1));
    obj(input)["idempotency_key"] = "escape";
    REQUIRE_FALSE(compile_child_fragment(json(input), r, envelope, remaining, 1));
    input = request({node("a", "model.native")});
    REQUIRE_FALSE(compile_child_fragment(json(input), r, envelope, remaining, 1));
    input = request();
    r.capabilities.at("algorithm.echo").adapter_revision = "changed";
    REQUIRE_FALSE(compile_child_fragment(json(input), r, envelope, remaining, 1));
}
TEST_CASE("B-SER01 arbitrary result roots keep strict numeric encoding limits",
          "[compiler][B-SER01]") {
    REQUIRE(parse_bounded_json_value("\"result\""));
    REQUIRE(parse_bounded_json_value("[true,null,3]"));
    REQUIRE_FALSE(parse_bounded_json_value("[1e-999]"));
    REQUIRE_FALSE(parse_bounded_json_value("[\"\\udc00\"]"));
    REQUIRE_FALSE(parse_bounded_json("[true,null,3]"));
}
TEST_CASE("A07 service handles remain typed input slots and cannot be literal plan constants",
          "[compiler][A07]") {
    auto r = registry();
    auto &cap = r.capabilities.at("algorithm.echo");
    cap.input_schema.properties.at("text").service_handle_type = "service.object/1";
    cap.handle_validator_revision = "local-validator/1";
    cap.handle_validator = [](const JsonValue &, std::string_view subject, std::uint64_t) {
        return Result<ValidatedHandleAccess>::success({std::string(subject), "object.a", 100});
    };
    auto input = request();
    REQUIRE_FALSE(Compiler{}.compile_submission(json(input), r));
    input = request({node("a", "algorithm.echo", reference("input", "handle"))});
    workflow(input)["inputs"] = JsonValue::Object{{"handle", schema_export(text_schema())}};
    obj(input)["input_values"] = JsonValue::Object{{"handle", "opaque-id"}};
    auto compiled = Compiler{}.compile_submission(json(input), r);
    REQUIRE(compiled);
    REQUIRE(compiled.value().plan->inputs.at("handle").service_handle_type == "service.object/1");
    REQUIRE(compiled.value().plan->canonical_export.find("opaque-id") == std::string::npos);
    JsonValue group = JsonValue::Object{
        {"id", "a"},
        {"type", "control.await"},
        {"with", JsonValue::Object{{"handle", literal("opaque-id")}}},
        {"control",
         JsonValue::Object{{"members", JsonValue::Array{node("child", "algorithm.echo",
                                                             reference("input", "handle"))}},
                           {"timeout_ms", 100.0}}}};
    REQUIRE_FALSE(Compiler{}.compile_submission(json(request({group})), r));
}
TEST_CASE("A01 semantic changes remain in fingerprint while JSON key order does not",
          "[compiler][A01]") {
    const std::string first =
        R"({"schema_version":"flamoris.submit/1","kind":"workflow","workflow":{"schema_version":"flamoris.workflow/0.1","workflow":{"id":"example"},"inputs":{},"nodes":[{"id":"a","type":"algorithm.echo","with":{"text":{"literal":"hello"}}}],"edges":[],"outputs":{"result":{"ref":{"source":"node","name":"a","path":[]}}},"limits":{}},"input_values":{}})";
    const std::string second =
        R"({"input_values":{},"workflow":{"limits":{},"outputs":{"result":{"ref":{"path":[],"name":"a","source":"node"}}},"edges":[],"nodes":[{"with":{"text":{"literal":"hello"}},"type":"algorithm.echo","id":"a"}],"inputs":{},"workflow":{"id":"example"},"schema_version":"flamoris.workflow/0.1"},"kind":"workflow","schema_version":"flamoris.submit/1"})";
    auto r = registry();
    auto a = Compiler{}.compile_submission(first, r), b = Compiler{}.compile_submission(second, r);
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE(a.value().plan->fingerprint == b.value().plan->fingerprint);
    REQUIRE(a.value().request_digest == b.value().request_digest);
    auto changed = request({node("a", "algorithm.echo", literal("changed"))});
    REQUIRE(Compiler{}.compile_submission(json(changed), r).value().plan->fingerprint !=
            a.value().plan->fingerprint);
}
TEST_CASE("A02 registered schemas cannot introduce recursion or oversized bounded types",
          "[compiler][A02]") {
    auto r = registry();
    r.capabilities.at("algorithm.echo").output_schema.max_bytes = 262145;
    REQUIRE_FALSE(Compiler{}.compile_submission(json(request()), r));
    r = registry();
    auto recursive = std::make_shared<ValueSchema>();
    recursive->kind = ValueSchema::Kind::array;
    recursive->max_items = 1;
    recursive->items = recursive;
    r.capabilities.at("algorithm.echo").output_schema = *recursive;
    REQUIRE_FALSE(fingerprint_capability(r.capabilities.at("algorithm.echo")));
    recursive->items.reset();
}
TEST_CASE("B-SER01 count and integer schemas reject fractional tokens rounded to integers",
          "[compiler][B-SER01]") {
    auto value = parse_bounded_json_value("1.00000000000000001");
    REQUIRE(value);
    REQUIRE(std::get<double>(value.value().data) == 1.0);
    ValueSchema integer;
    integer.kind = ValueSchema::Kind::integer;
    integer.minimum = 0;
    integer.maximum = 10;
    REQUIRE_FALSE(validate_value(value.value(), integer));
    REQUIRE(validate_value(parse_bounded_json_value("100e-2").value(), integer));
    auto raw = json(request());
    const auto offset = raw.find("\"limits\":{}");
    REQUIRE(offset != std::string::npos);
    raw.replace(offset, std::string("\"limits\":{}").size(),
                "\"limits\":{\"max_jobs\":2.00000000000000001}");
    REQUIRE_FALSE(validate_submission_identity(raw));
}
// RFC 8785 Appendix B: https://www.rfc-editor.org/rfc/rfc8785.html#appendix-B
// The reviewed Runtime profile intentionally rejects values above +/- (2^53-1).
TEST_CASE("B-SER01 RFC8785 Appendix B binary64 vectors and stricter profile ceiling",
          "[compiler][B-SER01]") {
    const std::pair<std::uint64_t, const char *> accepted[]{
        {0x0000000000000000ULL, "0"},
        {0x8000000000000000ULL, "0"},
        {0x0000000000000001ULL, "5e-324"},
        {0x8000000000000001ULL, "-5e-324"},
        {0x3eb0c6f7a0b5ed8cULL, "9.999999999999997e-7"},
        {0x3eb0c6f7a0b5ed8dULL, "0.000001"},
        {0x41b3de4355555553ULL, "333333333.3333332"},
        {0x41b3de4355555554ULL, "333333333.33333325"},
        {0x41b3de4355555555ULL, "333333333.3333333"},
        {0x41b3de4355555556ULL, "333333333.3333334"},
        {0x41b3de4355555557ULL, "333333333.33333343"},
        {0xbecbf647612f3696ULL, "-0.0000033333333333333333"},
        {0x43143ff3c1cb0959ULL, "1424953923781206.2"}};
    for (const auto &[bits, expected] : accepted) {
        auto encoded = canonical_json(JsonValue(std::bit_cast<double>(bits)));
        REQUIRE(encoded);
        REQUIRE(encoded.value() == expected);
    }
    const std::uint64_t rejected[]{
        0x7fefffffffffffffULL, 0xffefffffffffffffULL, 0x4340000000000000ULL, 0xc340000000000000ULL,
        0x4430000000000000ULL, 0x7fffffffffffffffULL, 0x7ff0000000000000ULL, 0x44b52d02c7e14af5ULL,
        0x44b52d02c7e14af6ULL, 0x44b52d02c7e14af7ULL, 0x444b1ae4d6e2ef4eULL, 0x444b1ae4d6e2ef4fULL,
        0x444b1ae4d6e2ef50ULL};
    for (auto bits : rejected)
        REQUIRE_FALSE(canonical_json(JsonValue(std::bit_cast<double>(bits))));
}
