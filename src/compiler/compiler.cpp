#include "internal.hpp"
#include <algorithm>
#include <functional>
#include <limits>
namespace flamoris::runtime {
using namespace compiler_detail;
namespace {
JsonValue effect_export(EffectSet effects) {
    JsonValue::Array a;
    for (auto n : effects.names())
        a.emplace_back(std::string(n));
    return a;
}
JsonValue capability_export(const CapabilityContract &c) {
    JsonValue::Object native;
    for (const auto &[k, v] : c.native_pins)
        native[k] = v;
    JsonValue::Object o{{"identifier", c.identifier},
                        {"version", c.version},
                        {"adapter_revision", c.adapter_revision},
                        {"input_schema", schema_export(c.input_schema)},
                        {"output_schema", schema_export(c.output_schema)},
                        {"effects", effect_export(c.effects)},
                        {"inference", c.inference},
                        {"retry_permitted", c.retry_permitted},
                        {"provider_deduplication", c.provider_deduplication},
                        {"cancellable", c.cancellable},
                        {"pausable", c.pausable},
                        {"max_attempts", integer(c.max_attempts)},
                        {"max_output_bytes", integer(c.max_output_bytes)},
                        {"max_timeout_ms", integer(c.max_timeout_ms)},
                        {"resource_units", integer(c.resource_units)},
                        {"native_pins", std::move(native)},
                        {"object_scope_fields", strings(c.object_scope_fields)},
                        {"handle_validator_revision", c.handle_validator_revision}};
    if (c.disjoint_scope)
        o["disjoint_scope"] = *c.disjoint_scope;
    return o;
}
JsonValue envelope_export(const ChildEnvelope &e) {
    JsonValue::Array pins;
    for (const auto &p : e.capabilities)
        pins.emplace_back(
            JsonValue::Object{{"identifier", p.identifier}, {"fingerprint", p.fingerprint}});
    return JsonValue::Object{{"identifier", e.identifier},
                             {"revision", e.revision},
                             {"capabilities", std::move(pins)},
                             {"max_children", integer(e.max_children)},
                             {"max_depth", integer(e.max_depth)},
                             {"max_attempts", integer(e.max_attempts)},
                             {"max_output_bytes", integer(e.max_output_bytes)}};
}
using LimitMember = std::uint64_t RunLimits::*;
const std::map<std::string, LimitMember> limit_members{
    {"timeout_ms", &RunLimits::timeout_ms},
    {"max_parallelism", &RunLimits::max_parallelism},
    {"max_jobs", &RunLimits::max_jobs},
    {"max_attempts", &RunLimits::max_attempts},
    {"max_dynamic_proposals", &RunLimits::max_dynamic_proposals},
    {"max_child_depth", &RunLimits::max_child_depth},
    {"max_output_bytes", &RunLimits::max_output_bytes},
    {"max_control_steps", &RunLimits::max_control_steps},
    {"max_suspensions", &RunLimits::max_suspensions},
    {"max_control_commands", &RunLimits::max_control_commands},
    {"max_resource_operations", &RunLimits::max_resource_operations},
    {"max_event_bytes", &RunLimits::max_event_bytes},
    {"max_trace_bytes", &RunLimits::max_trace_bytes}};
bool positive_limit(std::string_view name) {
    return name == "timeout_ms" || name == "max_parallelism" || name == "max_jobs" ||
           name == "max_attempts" || name == "max_control_steps" || name == "max_output_bytes";
}
RunLimits parse_limits(const JsonValue &v, const RunLimits &maximum) {
    RunLimits out = maximum;
    for (const auto &[name, value] : object(v)) {
        auto it = limit_members.find(name);
        if (it == limit_members.end())
            reject();
        auto n = count(value, positive_limit(name));
        if (n > maximum.*(it->second))
            reject(ErrorCode::budget_exceeded);
        out.*(it->second) = n;
    }
    for (const auto &[name, member] : limit_members)
        if (out.*member > 9007199254740991ULL || (positive_limit(name) && out.*member == 0))
            reject();
    return out;
}
JsonValue limits_export(const RunLimits &l) {
    JsonValue::Object o;
    for (const auto &[name, member] : limit_members)
        o[name] = integer(l.*member);
    return o;
}
std::vector<PathComponent> path(const JsonValue &v) {
    const auto &a = array(v);
    if (a.size() > 32)
        reject(ErrorCode::invalid_reference);
    std::vector<PathComponent> out;
    for (const auto &p : a) {
        if (auto *s = std::get_if<std::string>(&p.data)) {
            if (s->size() > 256)
                reject();
            out.emplace_back(*s);
        } else
            out.emplace_back(count(p));
    }
    return out;
}
JsonValue path_export(const std::vector<PathComponent> &p) {
    JsonValue::Array a;
    for (const auto &x : p)
        if (auto *s = std::get_if<std::string>(&x))
            a.emplace_back(*s);
        else
            a.push_back(integer(std::get<std::uint64_t>(x)));
    return a;
}
Binding binding(const JsonValue &v, std::size_t &refs) {
    const auto &o = object(v);
    Binding b;
    if (o.contains("literal")) {
        keys(o, {"literal"});
        b.literal = o.at("literal");
    } else {
        keys(o, {"ref"});
        if (++refs > 4096)
            reject();
        const auto &r = object(o.at("ref"));
        keys(r, {"source", "name", "path"});
        Reference ref;
        const auto &s = string(r.at("source"));
        if (s != "input" && s != "node")
            reject();
        ref.source = s == "input" ? Reference::Source::input : Reference::Source::node;
        ref.name = string(r.at("name"));
        if (!identifier(ref.name))
            reject(ErrorCode::invalid_reference);
        ref.path = path(r.at("path"));
        b.reference = std::move(ref);
    }
    return b;
}
JsonValue binding_export(const Binding &b) {
    if (b.literal)
        return JsonValue::Object{{"literal", *b.literal}};
    const auto &r = *b.reference;
    return JsonValue::Object{
        {"ref",
         JsonValue::Object{{"source", r.source == Reference::Source::input ? "input" : "node"},
                           {"name", r.name},
                           {"path", path_export(r.path)}}}};
}
JsonValue bindings_export(const std::map<std::string, Binding> &b) {
    JsonValue::Object o;
    for (const auto &[k, v] : b)
        o[k] = binding_export(v);
    return o;
}
AcceptanceExpression expression(const JsonValue &v, std::size_t &nodes, unsigned depth) {
    if (++nodes > 64 || depth > 8)
        reject();
    const auto &o = object(v);
    const auto &op = string(required(o, "op"));
    AcceptanceExpression e;
    using O = AcceptanceExpression::Op;
    if (op == "always") {
        keys(o, {"op"});
        e.op = O::always;
    } else if (op == "exists" || op == "eq") {
        if (op == "exists")
            keys(o, {"op", "path"});
        else
            keys(o, {"op", "path", "value"});
        e.op = op == "exists" ? O::exists : O::eq;
        e.path = path(o.at("path"));
        if (op == "eq") {
            const auto &x = o.at("value");
            if (std::holds_alternative<JsonValue::Array>(x.data) ||
                std::holds_alternative<JsonValue::Object>(x.data))
                reject();
            e.value = x;
        }
    } else if (op == "all" || op == "any") {
        keys(o, {"op", "args"});
        e.op = op == "all" ? O::all : O::any;
        const auto &a = array(o.at("args"));
        if (a.empty())
            reject();
        for (const auto &x : a)
            e.args.push_back(expression(x, nodes, depth + 1));
    } else if (op == "not") {
        keys(o, {"op", "arg"});
        e.op = O::negate;
        e.args.push_back(expression(o.at("arg"), nodes, depth + 1));
    } else
        reject();
    return e;
}
JsonValue expression_export(const AcceptanceExpression &e) {
    using O = AcceptanceExpression::Op;
    switch (e.op) {
    case O::always:
        return JsonValue::Object{{"op", "always"}};
    case O::exists:
        return JsonValue::Object{{"op", "exists"}, {"path", path_export(e.path)}};
    case O::eq:
        return JsonValue::Object{{"op", "eq"}, {"path", path_export(e.path)}, {"value", *e.value}};
    case O::negate:
        return JsonValue::Object{{"op", "not"}, {"arg", expression_export(e.args[0])}};
    case O::all:
    case O::any: {
        JsonValue::Array args;
        for (const auto &a : e.args)
            args.push_back(expression_export(a));
        return JsonValue::Object{{"op", e.op == O::all ? "all" : "any"}, {"args", std::move(args)}};
    }
    }
    reject();
}
void validate_expression(const AcceptanceExpression &e, const ValueSchema &s) {
    if ((e.op == AcceptanceExpression::Op::exists || e.op == AcceptanceExpression::Op::eq) &&
        !path_schema(s, e.path))
        reject(ErrorCode::invalid_reference);
    for (const auto &a : e.args)
        validate_expression(a, s);
}
struct Counters {
    std::size_t nodes{0}, refs{0}, groups{0};
    std::uint64_t attempts{0}, dynamic_jobs{0}, dynamic_attempts{0};
};
void add(std::uint64_t &v, std::uint64_t n) {
    if (n > 9007199254740991ULL - v)
        reject(ErrorCode::budget_exceeded);
    v += n;
}
// Structural validation is registry/profile-independent, and precedes submission claims.
void structure_node(const JsonValue &v, Counters &counts, unsigned depth) {
    if (++counts.nodes > 256 || depth > 8)
        reject();
    const auto &o = object(v);
    const auto &id = string(required(o, "id"));
    const auto &type = string(required(o, "type"));
    if (!identifier(id) || !identifier(type))
        reject();
    const bool group = type == "control.await" || type == "control.join" || type == "control.race";
    if (group) {
        keys(o, {"id", "type", "with", "control"});
        const auto &c = object(o.at("control"));
        if (type == "control.join")
            keys(c, {"members", "policy", "timeout_ms"});
        else if (type == "control.race")
            keys(c, {"members", "accept", "loser_policy", "timeout_ms"});
        else
            keys(c, {"members", "timeout_ms"});
        (void)count(c.at("timeout_ms"), true);
        const auto &m = array(c.at("members"));
        if (m.empty() || m.size() > 64 || (type == "control.await" && m.size() != 1))
            reject();
        std::set<std::string> ids;
        for (const auto &n : m) {
            structure_node(n, counts, depth + 1);
            if (!ids.insert(string(object(n).at("id"))).second)
                reject();
        }
        if (type == "control.join") {
            const auto &p = string(c.at("policy"));
            if (p != "all_success" && p != "all_settled")
                reject();
        }
        if (type == "control.race") {
            if (string(c.at("loser_policy")) != "cancel_unfinished")
                reject();
            std::size_t nodes = 0;
            (void)expression(c.at("accept"), nodes, 0);
        }
    } else {
        keys(o, {"id", "type", "with"}, {"timeout_ms", "retry", "child_policy"});
        if (o.contains("timeout_ms"))
            (void)count(o.at("timeout_ms"), true);
        if (o.contains("retry")) {
            const auto &r = object(o.at("retry"));
            keys(r, {"max_attempts", "backoff_ms"});
            if (count(r.at("max_attempts"), true) > 8)
                reject();
            (void)count(r.at("backoff_ms"));
        }
        if (o.contains("child_policy") && !identifier(string(o.at("child_policy"))))
            reject();
    }
    for (const auto &[k, b] : object(o.at("with"))) {
        (void)k;
        (void)binding(b, counts.refs);
    }
}
void structure_workflow(const JsonValue &w) {
    const auto &o = object(w);
    keys(o, {"schema_version", "workflow", "inputs", "nodes", "edges", "outputs", "limits"});
    if (string(o.at("schema_version")) != "flamoris.workflow/0.1")
        reject();
    const auto &meta = object(o.at("workflow"));
    keys(meta, {"id"}, {"name"});
    if (!identifier(string(meta.at("id"))))
        reject();
    if (meta.contains("name"))
        (void)string(meta.at("name"));
    for (const auto &[name, s] : object(o.at("inputs"))) {
        if (!identifier(name))
            reject();
        (void)schema(s);
    }
    const auto &nodes = array(o.at("nodes"));
    if (nodes.empty())
        reject();
    Counters counts;
    std::set<std::string> ids;
    for (const auto &n : nodes) {
        structure_node(n, counts, 0);
        if (!ids.insert(string(object(n).at("id"))).second)
            reject();
    }
    const auto &edges = array(o.at("edges"));
    if (edges.size() > 1024)
        reject();
    std::set<std::pair<std::string, std::string>> pairs;
    for (const auto &e : edges) {
        const auto &edge = object(e);
        keys(edge, {"from", "to"});
        auto from = string(edge.at("from")), to = string(edge.at("to"));
        if (!ids.contains(from) || !ids.contains(to) || !pairs.emplace(from, to).second)
            reject(ErrorCode::invalid_reference);
    }
    if (object(o.at("outputs")).empty())
        reject();
    for (const auto &[k, b] : object(o.at("outputs"))) {
        (void)k;
        (void)binding(b, counts.refs);
    }
    RunLimits max;
    for (const auto &[k, m] : limit_members)
        max.*m = 9007199254740991ULL;
    (void)parse_limits(o.at("limits"), max);
}
struct Parsed {
    JsonValue request;
    std::string kind, digest;
    std::optional<std::string> key;
};
Parsed parse_request(std::string_view text, JsonBounds bounds) {
    auto result = parse_bounded_json(text, bounds);
    if (!result)
        reject(ErrorCode::invalid_request);
    Parsed p;
    p.request = std::move(result.value());
    auto &o = std::get<JsonValue::Object>(p.request.data);
    if (string(required(o, "schema_version")) != "flamoris.submit/1")
        reject(ErrorCode::invalid_request);
    p.kind = string(required(o, "kind"));
    if (p.kind == "workflow") {
        keys(o, {"schema_version", "kind", "workflow", "input_values"}, {"idempotency_key"});
        structure_workflow(o.at("workflow"));
        (void)object(o.at("input_values"));
    } else if (p.kind == "inference") {
        keys(o, {"schema_version", "kind", "inference"}, {"idempotency_key"});
        const auto &i = object(o.at("inference"));
        keys(i, {"type", "with", "limits"}, {"child_policy"});
        if (!identifier(string(i.at("type"))))
            reject();
        (void)object(i.at("with"));
        RunLimits max;
        for (const auto &[k, m] : limit_members)
            max.*m = 9007199254740991ULL;
        (void)parse_limits(i.at("limits"), max);
        if (i.contains("child_policy") && !identifier(string(i.at("child_policy"))))
            reject();
    } else
        reject(ErrorCode::invalid_request);
    if (o.contains("idempotency_key")) {
        p.key = string(o.at("idempotency_key"));
        if (p.key->empty() || p.key->size() > 256)
            reject(ErrorCode::invalid_request);
    }
    auto digest_value = p.request;
    auto &d = std::get<JsonValue::Object>(digest_value.data);
    d.erase("idempotency_key");
    if (p.kind == "workflow")
        std::get<JsonValue::Object>(d.at("workflow").data).erase("workflow");
    auto digest = domain_digest("flamoris.submission/1\n", digest_value);
    if (!digest)
        reject(ErrorCode::internal_error);
    p.digest = std::move(digest.value());
    return p;
}
bool has_handle(const ValueSchema &s) {
    if (s.service_handle_type)
        return true;
    for (const auto &[key, child] : s.properties) {
        (void)key;
        if (has_handle(child))
            return true;
    }
    if (s.items && has_handle(*s.items))
        return true;
    for (const auto &child : s.tuple_items)
        if (has_handle(child))
            return true;
    for (const auto &child : s.variants)
        if (has_handle(child))
            return true;
    return false;
}
ValueSchema with_handle_types(ValueSchema source, const ValueSchema &target) {
    source.service_handle_type = target.service_handle_type;
    for (auto &[key, child] : source.properties) {
        auto t = target.properties.find(key);
        if (t != target.properties.end())
            child = with_handle_types(child, t->second);
    }
    if (source.items && target.items)
        source.items =
            std::make_shared<const ValueSchema>(with_handle_types(*source.items, *target.items));
    return source;
}
ValueSchema without_handle_types(ValueSchema source) {
    source.service_handle_type.reset();
    for (auto &[key, child] : source.properties) {
        (void)key;
        child = without_handle_types(child);
    }
    if (source.items)
        source.items = std::make_shared<const ValueSchema>(without_handle_types(*source.items));
    return source;
}
ValueSchema literal_schema(const JsonValue &v) {
    ValueSchema s;
    s.from_literal = true;
    if (std::holds_alternative<std::nullptr_t>(v.data))
        return s;
    if (std::holds_alternative<bool>(v.data)) {
        s.kind = ValueSchema::Kind::boolean;
        return s;
    }
    if (auto *n = std::get_if<double>(&v.data)) {
        s.kind = v.exact_integer && std::floor(*n) == *n ? ValueSchema::Kind::integer
                                                         : ValueSchema::Kind::number;
        s.minimum = s.maximum = *n;
        return s;
    }
    if (auto *t = std::get_if<std::string>(&v.data)) {
        s.kind = ValueSchema::Kind::string;
        s.max_bytes = t->size();
        return s;
    }
    if (auto *a = std::get_if<JsonValue::Array>(&v.data)) {
        s.kind = ValueSchema::Kind::array;
        s.max_items = a->size();
        for (const auto &x : *a)
            s.tuple_items.push_back(literal_schema(x));
        return s;
    }
    s.kind = ValueSchema::Kind::object;
    for (const auto &[k, x] : std::get<JsonValue::Object>(v.data)) {
        s.properties.emplace(k, literal_schema(x));
        s.required.insert(k);
    }
    return s;
}
ValueSchema binding_schema(const Binding &b, const std::map<std::string, ValueSchema> &inputs,
                           const std::map<std::string, ValueSchema> &outputs, bool grouped) {
    if (b.literal)
        return literal_schema(*b.literal);
    const auto &r = *b.reference;
    if (grouped && r.source == Reference::Source::node)
        reject(ErrorCode::invalid_reference);
    const auto &source = r.source == Reference::Source::input ? inputs : outputs;
    auto it = source.find(r.name);
    if (it == source.end())
        reject(ErrorCode::invalid_reference);
    const auto *s = path_schema(it->second, r.path);
    if (!s)
        reject(ErrorCode::invalid_reference);
    return *s;
}
JsonValue step_export(const PlanStep &s) {
    JsonValue::Array members;
    for (const auto &m : s.members)
        members.push_back(step_export(m));
    JsonValue::Object o{{"id", s.id},
                        {"type", s.type},
                        {"structural_path", s.structural_path},
                        {"with", bindings_export(s.inputs)},
                        {"dependencies", strings(s.dependencies)},
                        {"members", std::move(members)},
                        {"output_schema", schema_export(s.output_schema)},
                        {"effects", effect_export(s.effects)},
                        {"timeout_ms", integer(s.timeout_ms)},
                        {"max_attempts", integer(s.max_attempts)},
                        {"backoff_ms", integer(s.backoff_ms)},
                        {"group_policy", s.group_policy},
                        {"pause_supported", s.pause_supported}};
    if (s.capability_pin)
        o["pin"] = JsonValue::Object{{"identifier", s.capability_pin->identifier},
                                     {"fingerprint", s.capability_pin->fingerprint}};
    if (s.child_envelope)
        o["child_envelope"] = envelope_export(*s.child_envelope);
    else
        o["child_envelope"] = nullptr;
    if (s.acceptance)
        o["accept"] = expression_export(*s.acceptance);
    return o;
}
ValueSchema settled_schema(const ValueSchema &value) {
    ValueSchema status;
    status.kind = ValueSchema::Kind::string;
    status.max_bytes = 16;
    ValueSchema text;
    text.kind = ValueSchema::Kind::string;
    text.max_bytes = 256;
    ValueSchema codes;
    codes.kind = ValueSchema::Kind::array;
    codes.max_items = 8;
    codes.items = std::make_shared<const ValueSchema>(text);
    ValueSchema error;
    error.kind = ValueSchema::Kind::object;
    for (const auto *k : {"schema_version", "code", "category", "message", "stage",
                          "external_outcome", "retry_disposition"}) {
        error.properties[k] = text;
        error.required.insert(k);
    }
    error.properties["cause_codes"] = codes;
    error.required.insert("cause_codes");
    for (const auto *k : {"reason", "run_id", "job_id", "attempt_id", "diagnostic_ref"})
        error.properties[k] = text;
    ValueSchema ok;
    ok.kind = ValueSchema::Kind::object;
    ok.properties = {{"status", status}, {"value", value}};
    ok.required = {"status", "value"};
    ValueSchema bad;
    bad.kind = ValueSchema::Kind::object;
    bad.properties = {{"status", status}, {"error", error}};
    bad.required = {"status", "error"};
    ValueSchema outcome;
    outcome.kind = ValueSchema::Kind::union_value;
    outcome.variants = {ok, bad};
    return outcome;
}
struct Build {
    const CapabilitySnapshot &registry;
    RunLimits limits;
    Counters counters;
    std::map<std::string, CapabilityPin> pins;
    void pin(const CapabilityContract &c) {
        auto p = fingerprint_capability(c);
        if (!p)
            reject(p.error().code());
        pins[c.identifier] = p.value();
    }
    bool disjoint(const PlanStep &a, const PlanStep &b) const {
        if (!a.capability_pin || !b.capability_pin || a.child_envelope || b.child_envelope)
            return false;
        const auto &x = registry.capabilities.at(a.type);
        const auto &y = registry.capabilities.at(b.type);
        return x.disjoint_scope && y.disjoint_scope && *x.disjoint_scope != *y.disjoint_scope;
    }
    bool conflict(const PlanStep &a, const PlanStep &b) const {
        auto write = [](const EffectSet &e) { return e.contains(Effect::write); };
        auto access = [](const EffectSet &e) {
            return e.contains(Effect::read) || e.contains(Effect::write);
        };
        return ((write(a.effects) && access(b.effects)) ||
                (write(b.effects) && access(a.effects))) &&
               !disjoint(a, b);
    }
    PlanStep node(const JsonValue &value, const std::map<std::string, ValueSchema> &inputs,
                  const std::map<std::string, ValueSchema> &outputs, bool grouped,
                  std::string prefix, unsigned depth) {
        if (++counters.nodes > 256 || depth > 8)
            reject();
        const auto &o = object(value);
        PlanStep s;
        s.id = string(o.at("id"));
        s.type = string(o.at("type"));
        s.structural_path = prefix + s.id;
        s.timeout_ms = limits.timeout_ms;
        std::map<std::string, ValueSchema> bound_schemas;
        for (const auto &[key, v] : object(o.at("with"))) {
            auto b = binding(v, counters.refs);
            bound_schemas.emplace(key, binding_schema(b, inputs, outputs, grouped));
            s.inputs.emplace(key, std::move(b));
        }
        const bool group =
            s.type == "control.await" || s.type == "control.join" || s.type == "control.race";
        if (group) {
            ++counters.groups;
            const auto &c = object(o.at("control"));
            s.timeout_ms = std::min(count(c.at("timeout_ms"), true), limits.timeout_ms);
            s.group_policy = s.type == "control.race"    ? "race"
                             : s.type == "control.await" ? "all_success"
                                                         : string(c.at("policy"));
            std::vector<EffectSet> effects;
            for (const auto &v : array(c.at("members"))) {
                auto m = node(v, bound_schemas, {}, true, s.structural_path + "/", depth + 1);
                effects.push_back(m.effects);
                s.members.push_back(std::move(m));
            }
            for (std::size_t i = 0; i < s.members.size(); ++i)
                for (std::size_t j = i + 1; j < s.members.size(); ++j)
                    if (conflict(s.members[i], s.members[j]))
                        reject();
            auto effect = EffectSet::aggregate(effects);
            if (!effect)
                reject();
            s.effects = effect.value();
            s.output_schema.kind = ValueSchema::Kind::object;
            if (s.type == "control.race") {
                if (s.effects.contains(Effect::write) || s.effects.contains(Effect::destructive))
                    reject();
                std::size_t count_expr = 0;
                s.acceptance = expression(c.at("accept"), count_expr, 0);
                ValueSchema values;
                values.kind = ValueSchema::Kind::union_value;
                for (const auto &m : s.members) {
                    validate_expression(*s.acceptance, m.output_schema);
                    values.variants.push_back(m.output_schema);
                }
                ValueSchema participant;
                participant.kind = ValueSchema::Kind::string;
                participant.max_bytes = 128;
                s.output_schema.properties = {{"participant", participant}, {"value", values}};
                s.output_schema.required = {"participant", "value"};
            } else {
                ValueSchema result;
                result.kind = ValueSchema::Kind::array;
                result.max_items = s.members.size();
                for (const auto &m : s.members)
                    result.tuple_items.push_back(s.group_policy == "all_settled"
                                                     ? settled_schema(m.output_schema)
                                                     : m.output_schema);
                const std::string field = s.group_policy == "all_settled" ? "outcomes" : "results";
                s.output_schema.properties[field] = std::move(result);
                s.output_schema.required.insert(field);
            }
        } else {
            auto it = registry.capabilities.find(s.type);
            if (it == registry.capabilities.end())
                reject(ErrorCode::unknown_capability);
            const auto &c = it->second;
            if (c.identifier != s.type || !identifier(c.identifier) || c.version.empty() ||
                c.adapter_revision.empty() || c.max_attempts == 0 || c.max_attempts > 8 ||
                c.max_timeout_ms == 0 || c.max_output_bytes == 0)
                reject();
            if (c.input_schema.kind != ValueSchema::Kind::object)
                reject();
            pin(c);
            s.capability_pin = pins.at(c.identifier);
            s.output_schema = c.output_schema;
            s.effects = c.effects;
            s.pause_supported = c.pausable;
            s.timeout_ms = std::min(limits.timeout_ms, c.max_timeout_ms);
            if (o.contains("timeout_ms"))
                s.timeout_ms = std::min(s.timeout_ms, count(o.at("timeout_ms"), true));
            for (const auto &required_input : c.input_schema.required)
                if (!s.inputs.contains(required_input))
                    reject(ErrorCode::invalid_reference);
            for (auto &[key, source] : bound_schemas) {
                auto target = c.input_schema.properties.find(key);
                if (target != c.input_schema.properties.end() && has_handle(target->second)) {
                    const auto &bind = s.inputs.at(key);
                    if (!c.handle_validator || c.handle_validator_revision.empty() ||
                        !bind.reference || source.from_literal)
                        reject(ErrorCode::invalid_reference);
                    if (bind.reference->source == Reference::Source::input)
                        source = with_handle_types(source, target->second);
                }
                if (target == c.input_schema.properties.end() ||
                    !schema_assignable(source, target->second) ||
                    (has_handle(target->second) && s.inputs.at(key).literal))
                    reject(ErrorCode::invalid_reference);
            }
            if (o.contains("retry")) {
                const auto &r = object(o.at("retry"));
                s.max_attempts = count(r.at("max_attempts"), true);
                s.backoff_ms = count(r.at("backoff_ms"));
                if (s.max_attempts > c.max_attempts || (s.max_attempts > 1 && !c.retry_permitted))
                    reject();
            }
            add(counters.attempts, s.max_attempts);
            if (o.contains("child_policy")) {
                if (!c.inference)
                    reject();
                auto cp = registry.child_policies.find(string(o.at("child_policy")));
                if (cp == registry.child_policies.end())
                    reject(ErrorCode::unknown_capability);
                s.child_envelope = cp->second;
                const auto &e = *s.child_envelope;
                if (e.identifier != cp->first || e.revision.empty() || e.max_children == 0 ||
                    e.max_depth == 0 || e.max_depth > limits.max_child_depth ||
                    e.max_attempts == 0 || e.max_output_bytes == 0 ||
                    e.max_output_bytes > limits.max_output_bytes || e.capabilities.empty())
                    reject();
                std::vector<EffectSet> effects{s.effects};
                for (const auto &p : e.capabilities) {
                    auto cap = registry.capabilities.find(p.identifier);
                    if (cap == registry.capabilities.end())
                        reject(ErrorCode::unknown_capability);
                    pin(cap->second);
                    if (pins.at(p.identifier) != p)
                        reject(ErrorCode::plan_stale);
                    effects.push_back(cap->second.effects);
                }
                auto effect = EffectSet::aggregate(effects);
                if (!effect)
                    reject();
                s.effects = effect.value();
                add(counters.dynamic_jobs, e.max_children);
                add(counters.dynamic_attempts, e.max_attempts);
            }
        }
        return s;
    }
};
JsonValue normalize_direct(const JsonValue &inference, const CapabilitySnapshot &registry,
                           JsonValue::Object &values) {
    const auto &i = object(inference);
    const auto &type = string(i.at("type"));
    auto it = registry.capabilities.find(type);
    if (it == registry.capabilities.end())
        reject(ErrorCode::unknown_capability);
    const auto &cap = it->second;
    if (!cap.inference || cap.input_schema.kind != ValueSchema::Kind::object)
        reject(ErrorCode::unsupported_model);
    values = object(i.at("with"));
    if (!validate_value(values, cap.input_schema))
        reject();
    JsonValue::Object inputs, bindings;
    for (const auto &[name, value] : values) {
        (void)value;
        inputs[name] = schema_export(without_handle_types(cap.input_schema.properties.at(name)));
        bindings[name] = JsonValue::Object{
            {"ref",
             JsonValue::Object{{"source", "input"}, {"name", name}, {"path", JsonValue::Array{}}}}};
    }
    JsonValue::Object node{{"id", "inference"}, {"type", type}, {"with", std::move(bindings)}};
    if (i.contains("child_policy"))
        node["child_policy"] = i.at("child_policy");
    return JsonValue::Object{
        {"schema_version", "flamoris.workflow/0.1"},
        {"workflow", JsonValue::Object{{"id", "direct"}}},
        {"inputs", std::move(inputs)},
        {"nodes", JsonValue::Array{JsonValue(std::move(node))}},
        {"edges", JsonValue::Array{}},
        {"outputs",
         JsonValue::Object{
             {"result",
              JsonValue::Object{{"ref", JsonValue::Object{{"source", "node"},
                                                          {"name", "inference"},
                                                          {"path", JsonValue::Array{}}}}}}}},
        {"limits", i.at("limits")}};
}
std::shared_ptr<const ExecutionPlan> compile(const JsonValue &workflow,
                                             const JsonValue::Object &values,
                                             const CapabilitySnapshot &registry,
                                             const CompilerProfile &profile) {
    const auto &w = object(workflow);
    auto plan = std::make_shared<ExecutionPlan>();
    plan->profile_revision = profile.revision;
    plan->limits = parse_limits(w.at("limits"), profile.limits);
    Build b{registry, plan->limits, {}, {}};
    ValueSchema input_schema;
    input_schema.kind = ValueSchema::Kind::object;
    for (const auto &[name, v] : object(w.at("inputs"))) {
        auto s = schema(v);
        plan->inputs.emplace(name, s);
        input_schema.properties.emplace(name, s);
        input_schema.required.insert(name);
    }
    if (!validate_value(values, input_schema))
        reject(ErrorCode::invalid_reference);
    std::map<std::string, const JsonValue *> raw;
    std::map<std::string, std::set<std::string>> deps;
    for (const auto &n : array(w.at("nodes"))) {
        auto id = string(object(n).at("id"));
        raw.emplace(id, &n);
        deps[id];
    }
    std::size_t refs = 0;
    for (const auto &[id, node] : raw)
        for (const auto &[key, v] : object(object(*node).at("with"))) {
            (void)key;
            auto bind = binding(v, refs);
            if (bind.reference && bind.reference->source == Reference::Source::node) {
                if (!raw.contains(bind.reference->name))
                    reject(ErrorCode::invalid_reference);
                deps[id].insert(bind.reference->name);
            }
        }
    for (const auto &e : array(w.at("edges"))) {
        const auto &o = object(e);
        deps.at(string(o.at("to"))).insert(string(o.at("from")));
    }
    std::size_t edge_count = 0;
    for (const auto &[id, d] : deps) {
        (void)id;
        edge_count += d.size();
    }
    if (edge_count > 1024)
        reject();
    std::map<std::string, ValueSchema> outputs;
    std::set<std::string> ready;
    std::set<std::string> done;
    while (done.size() < raw.size()) {
        ready.clear();
        for (const auto &[id, d] : deps)
            if (!done.contains(id) &&
                std::all_of(d.begin(), d.end(), [&](const auto &x) { return done.contains(x); }))
                ready.insert(id);
        if (ready.empty())
            reject(ErrorCode::invalid_reference);
        auto id = *ready.begin();
        auto step = b.node(*raw.at(id), plan->inputs, outputs, false, "", 0);
        step.dependencies.assign(deps[id].begin(), deps[id].end());
        if (step.capability_pin) {
            const auto &cap = registry.capabilities.at(step.type);
            for (const auto &[key, bind] : step.inputs)
                if (bind.reference && bind.reference->source == Reference::Source::input &&
                    bind.reference->path.empty()) {
                    auto it = cap.input_schema.properties.find(key);
                    if (it != cap.input_schema.properties.end() && has_handle(it->second))
                        plan->inputs.at(bind.reference->name) =
                            with_handle_types(plan->inputs.at(bind.reference->name), it->second);
                }
        }
        outputs[id] = step.output_schema;
        plan->steps.push_back(std::move(step));
        done.insert(id);
    }
    std::map<std::string, std::set<std::string>> ancestors;
    for (const auto &step : plan->steps) {
        auto &reachable = ancestors[step.id];
        for (const auto &parent : step.dependencies) {
            reachable.insert(parent);
            reachable.insert(ancestors[parent].begin(), ancestors[parent].end());
        }
    }
    auto precedes = [&](const std::string &from, const std::string &to) {
        return ancestors.at(to).contains(from);
    };
    for (std::size_t i = 0; i < plan->steps.size(); ++i)
        for (std::size_t j = i + 1; j < plan->steps.size(); ++j)
            if (b.conflict(plan->steps[i], plan->steps[j]) &&
                !precedes(plan->steps[i].id, plan->steps[j].id) &&
                !precedes(plan->steps[j].id, plan->steps[i].id))
                reject();
    for (const auto &[name, v] : object(w.at("outputs"))) {
        auto bind = binding(v, b.counters.refs);
        (void)binding_schema(bind, plan->inputs, outputs, false);
        plan->outputs.emplace(name, std::move(bind));
    }
    plan->single_root_inference = plan->steps.size() == 1 && plan->steps[0].capability_pin &&
                                  registry.capabilities.at(plan->steps[0].type).inference;
    plan->static_jobs = b.counters.nodes + (plan->single_root_inference ? 0 : 1);
    plan->static_attempts = b.counters.attempts;
    std::uint64_t jobs = plan->static_jobs;
    add(jobs, b.counters.dynamic_jobs);
    std::uint64_t attempts = plan->static_attempts;
    add(attempts, b.counters.dynamic_attempts);
    if (jobs > plan->limits.max_jobs || attempts > plan->limits.max_attempts ||
        b.counters.groups > plan->limits.max_control_steps)
        reject(ErrorCode::budget_exceeded);
    std::vector<EffectSet> effects;
    for (const auto &s : plan->steps)
        effects.push_back(s.effects);
    auto aggregate = EffectSet::aggregate(effects);
    if (!aggregate)
        reject();
    plan->effects = aggregate.value();
    for (const auto &[id, pin] : b.pins) {
        (void)id;
        plan->pins.push_back(pin);
    }
    std::uint64_t slots = 32;
    auto charge = [&](std::uint64_t count, std::uint64_t factor) {
        if (count > 9007199254740991ULL / factor)
            reject(ErrorCode::budget_exceeded);
        add(slots, count * factor);
    };
    charge(plan->limits.max_jobs, 16);
    charge(plan->limits.max_attempts, 12);
    charge(plan->limits.max_suspensions, 8);
    charge(plan->limits.max_control_commands, 8);
    charge(b.counters.groups + b.counters.dynamic_jobs, 8);
    charge(plan->limits.max_resource_operations, 8);
    charge(plan->limits.max_dynamic_proposals, 8);
    charge(plan->limits.max_resource_operations, 12);
    if (slots > 9007199254740991ULL / 8192)
        reject(ErrorCode::budget_exceeded);
    plan->mandatory_event_bytes = slots * 8192;
    if (plan->mandatory_event_bytes > plan->limits.max_event_bytes)
        reject(ErrorCode::budget_exceeded);
    JsonValue::Array steps;
    for (const auto &s : plan->steps)
        steps.push_back(step_export(s));
    JsonValue::Object schemas;
    for (const auto &[k, s] : plan->inputs)
        schemas[k] = schema_export(s);
    JsonValue export_value =
        JsonValue::Object{{"schema_revision", plan->schema_revision},
                          {"compiler_revision", plan->compiler_revision},
                          {"canonicalization_revision", "rfc8785/1"},
                          {"profile_revision", plan->profile_revision},
                          {"scheduler_revision", "flamoris.scheduler/1"},
                          {"event_reservation_revision", "flamoris.event-reservation/1"},
                          {"steps", std::move(steps)},
                          {"inputs", std::move(schemas)},
                          {"outputs", bindings_export(plan->outputs)},
                          {"limits", limits_export(plan->limits)},
                          {"effects", effect_export(plan->effects)},
                          {"single_root_inference", plan->single_root_inference},
                          {"mandatory_event_bytes", integer(plan->mandatory_event_bytes)}};
    auto canonical = canonical_json(export_value);
    auto digest = domain_digest("flamoris.plan/1\n", export_value);
    if (!canonical || !digest)
        reject(ErrorCode::internal_error);
    plan->canonical_export = std::move(canonical.value());
    plan->fingerprint = std::move(digest.value());
    return plan;
}
} // namespace
Result<CapabilityPin> fingerprint_capability(const CapabilityContract &c) {
    try {
        if (!identifier(c.identifier) || c.version.empty() || c.adapter_revision.empty())
            reject();
        auto digest = domain_digest("flamoris.capability/1\n", capability_export(c));
        if (!digest)
            return Result<CapabilityPin>::failure(digest.error());
        return Result<CapabilityPin>::success({c.identifier, std::move(digest.value())});
    } catch (...) {
        return Result<CapabilityPin>::failure(ErrorEnvelope::make(ErrorCode::invalid_workflow));
    }
}
Result<SubmissionIdentity> validate_submission_identity(std::string_view json) {
    try {
        auto p = parse_request(json, {});
        return Result<SubmissionIdentity>::success({p.kind, p.digest, p.key});
    } catch (const Failure &f) {
        return Result<SubmissionIdentity>::failure(ErrorEnvelope::make(f.code));
    } catch (...) {
        return Result<SubmissionIdentity>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
    }
}
Result<CompiledSubmission> Compiler::compile_submission(std::string_view json,
                                                        const CapabilitySnapshot &registry) const {
    try {
        auto p = parse_request(json, profile_.json_bounds);
        const auto &o = object(p.request);
        CompiledSubmission out;
        out.request_kind = p.kind;
        out.request_digest = p.digest;
        out.idempotency_key = p.key;
        JsonValue workflow;
        if (p.kind == "workflow") {
            workflow = o.at("workflow");
            out.input_values = object(o.at("input_values"));
        } else
            workflow = normalize_direct(o.at("inference"), registry, out.input_values);
        out.plan = compile(workflow, out.input_values, registry, profile_);
        return Result<CompiledSubmission>::success(std::move(out));
    } catch (const Failure &f) {
        return Result<CompiledSubmission>::failure(ErrorEnvelope::make(f.code));
    } catch (...) {
        return Result<CompiledSubmission>::failure(ErrorEnvelope::make(ErrorCode::internal_error));
    }
}
Result<CompiledSubmission> compile_child_fragment(std::string_view json,
                                                  const CapabilitySnapshot &registry,
                                                  const ChildEnvelope &envelope,
                                                  const RunLimits &remaining,
                                                  std::uint64_t child_depth) {
    try {
        if (child_depth == 0 || child_depth > envelope.max_depth ||
            child_depth > remaining.max_child_depth || envelope.max_children == 0 ||
            envelope.max_attempts == 0 || envelope.max_output_bytes == 0 ||
            envelope.capabilities.empty())
            reject(ErrorCode::budget_exceeded);
        CompilerProfile profile;
        profile.revision = "flamoris.child-profile/1";
        for (const auto &[name, member] : limit_members) {
            (void)name;
            profile.limits.*member = std::min(profile.limits.*member, remaining.*member);
        }
        profile.limits.max_jobs = std::min(profile.limits.max_jobs, envelope.max_children);
        profile.limits.max_attempts = std::min(profile.limits.max_attempts, envelope.max_attempts);
        profile.limits.max_output_bytes =
            std::min(profile.limits.max_output_bytes, envelope.max_output_bytes);
        profile.limits.max_child_depth =
            std::min(envelope.max_depth - child_depth, remaining.max_child_depth - child_depth);
        auto compiled = Compiler(profile).compile_submission(json, registry);
        if (!compiled)
            return compiled;
        if (compiled.value().idempotency_key)
            reject(ErrorCode::invalid_request);
        for (const auto &pin : compiled.value().plan->pins) {
            auto allowed =
                std::find(envelope.capabilities.begin(), envelope.capabilities.end(), pin);
            if (allowed == envelope.capabilities.end())
                reject(ErrorCode::permission_denied);
        }
        return compiled;
    } catch (const Failure &failure) {
        return Result<CompiledSubmission>::failure(ErrorEnvelope::make(failure.code));
    } catch (...) {
        return Result<CompiledSubmission>::failure(ErrorEnvelope::make(ErrorCode::internal_error));
    }
}
Result<void> verify_plan_pins(const ExecutionPlan &plan, const CapabilitySnapshot &registry,
                              bool available) {
    try {
        for (const auto &pin : plan.pins) {
            auto it = registry.capabilities.find(pin.identifier);
            if (it == registry.capabilities.end())
                return Result<void>::failure(ErrorEnvelope::make(ErrorCode::plan_stale));
            auto current = fingerprint_capability(it->second);
            if (!current || current.value() != pin)
                return Result<void>::failure(ErrorEnvelope::make(ErrorCode::plan_stale));
            if (available && !it->second.available)
                return Result<void>::failure(
                    ErrorEnvelope::make(ErrorCode::capability_unavailable));
        }
        std::function<bool(const PlanStep &)> check = [&](const PlanStep &s) {
            if (s.child_envelope) {
                auto it = registry.child_policies.find(s.child_envelope->identifier);
                if (it == registry.child_policies.end())
                    return false;
                auto a = canonical_json(envelope_export(it->second)),
                     b = canonical_json(envelope_export(*s.child_envelope));
                if (!a || !b || a.value() != b.value())
                    return false;
            }
            for (const auto &m : s.members)
                if (!check(m))
                    return false;
            return true;
        };
        for (const auto &s : plan.steps)
            if (!check(s))
                return Result<void>::failure(ErrorEnvelope::make(ErrorCode::plan_stale));
        return Result<void>::success();
    } catch (...) {
        return Result<void>::failure(ErrorEnvelope::make(ErrorCode::internal_error));
    }
}
} // namespace flamoris::runtime
