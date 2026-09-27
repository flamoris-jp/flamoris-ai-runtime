#include "catch_amalgamated.hpp"
#include "flamoris/runtime/compiler.hpp"
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
