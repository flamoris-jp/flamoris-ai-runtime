#include "internal.hpp"
#include <algorithm>
namespace flamoris::runtime {
using namespace compiler_detail;
namespace compiler_detail {
ValueSchema schema(const JsonValue &v, unsigned depth) {
    if (depth > 16)
        reject();
    const auto &o = object(v);
    const auto &type = string(required(o, "type"));
    ValueSchema s;
    if (type == "null" || type == "boolean") {
        keys(o, {"type"});
        s.kind = type == "null" ? ValueSchema::Kind::null_value : ValueSchema::Kind::boolean;
    } else if (type == "integer" || type == "number") {
        keys(o, {"type", "minimum", "maximum"});
        s.kind = type == "integer" ? ValueSchema::Kind::integer : ValueSchema::Kind::number;
        s.minimum = numeric(o.at("minimum"));
        s.maximum = numeric(o.at("maximum"));
        if (s.minimum > s.maximum ||
            (type == "integer" &&
             (!o.at("minimum").exact_integer || !o.at("maximum").exact_integer ||
              std::floor(s.minimum) != s.minimum || std::floor(s.maximum) != s.maximum)))
            reject();
    } else if (type == "string") {
        keys(o, {"type", "max_bytes"});
        s.kind = ValueSchema::Kind::string;
        s.max_bytes = count(o.at("max_bytes"));
        if (s.max_bytes > 262144)
            reject();
    } else if (type == "array") {
        keys(o, {"type", "items", "max_items"});
        s.kind = ValueSchema::Kind::array;
        s.max_items = count(o.at("max_items"));
        if (s.max_items > 65536)
            reject();
        s.items = std::make_shared<const ValueSchema>(schema(o.at("items"), depth + 1));
    } else if (type == "object") {
        keys(o, {"type", "properties", "required", "additional_properties"});
        if (!std::holds_alternative<bool>(o.at("additional_properties").data) ||
            std::get<bool>(o.at("additional_properties").data))
            reject();
        s.kind = ValueSchema::Kind::object;
        for (const auto &[key, value] : object(o.at("properties")))
            s.properties.emplace(key, schema(value, depth + 1));
        for (const auto &name : array(o.at("required"))) {
            const auto &key = string(name);
            if (!s.properties.contains(key) || !s.required.insert(key).second)
                reject();
        }
    } else
        reject();
    return s;
}
const ValueSchema *path_schema(const ValueSchema &root, const std::vector<PathComponent> &path) {
    const ValueSchema *current = &root;
    for (const auto &part : path) {
        if (const auto *key = std::get_if<std::string>(&part)) {
            if (current->kind != ValueSchema::Kind::object)
                return nullptr;
            auto it = current->properties.find(*key);
            if (it == current->properties.end())
                return nullptr;
            current = &it->second;
        } else {
            auto index = std::get<std::uint64_t>(part);
            if (current->kind != ValueSchema::Kind::array || index >= current->max_items)
                return nullptr;
            if (!current->tuple_items.empty()) {
                if (index >= current->tuple_items.size())
                    return nullptr;
                current = &current->tuple_items[static_cast<std::size_t>(index)];
            } else {
                if (!current->items)
                    return nullptr;
                current = current->items.get();
            }
        }
    }
    return current;
}
const JsonValue *path_value(const JsonValue &root, const std::vector<PathComponent> &path) {
    const JsonValue *current = &root;
    for (const auto &part : path) {
        if (const auto *key = std::get_if<std::string>(&part)) {
            auto *o = std::get_if<JsonValue::Object>(&current->data);
            if (!o)
                return nullptr;
            auto it = o->find(*key);
            if (it == o->end())
                return nullptr;
            current = &it->second;
        } else {
            auto *a = std::get_if<JsonValue::Array>(&current->data);
            auto i = std::get<std::uint64_t>(part);
            if (!a || i >= a->size())
                return nullptr;
            current = &(*a)[static_cast<std::size_t>(i)];
        }
    }
    return current;
}
} // namespace compiler_detail
Result<ValueSchema> parse_value_schema(const JsonValue &v) {
    try {
        return Result<ValueSchema>::success(schema(v));
    } catch (...) {
        return Result<ValueSchema>::failure(ErrorEnvelope::make(ErrorCode::invalid_workflow));
    }
}
namespace {
bool valid(const JsonValue &v, const ValueSchema &s, unsigned depth) {
    if (depth > 32)
        return false;
    using K = ValueSchema::Kind;
    switch (s.kind) {
    case K::null_value:
        return std::holds_alternative<std::nullptr_t>(v.data);
    case K::boolean:
        return std::holds_alternative<bool>(v.data);
    case K::integer:
    case K::number: {
        auto *n = std::get_if<double>(&v.data);
        return n && std::isfinite(*n) && *n >= s.minimum && *n <= s.maximum &&
               (s.kind != K::integer || (v.exact_integer && std::floor(*n) == *n));
    }
    case K::string: {
        auto *t = std::get_if<std::string>(&v.data);
        return t && t->size() <= s.max_bytes;
    }
    case K::array: {
        auto *a = std::get_if<JsonValue::Array>(&v.data);
        if (!a || a->size() > s.max_items)
            return false;
        for (std::size_t i = 0; i < a->size(); ++i) {
            auto *item = s.tuple_items.empty()
                             ? s.items.get()
                             : (i < s.tuple_items.size() ? &s.tuple_items[i] : nullptr);
            if (!item || !valid((*a)[i], *item, depth + 1))
                return false;
        }
        return true;
    }
    case K::object: {
        auto *o = std::get_if<JsonValue::Object>(&v.data);
        if (!o)
            return false;
        for (const auto &r : s.required)
            if (!o->contains(r))
                return false;
        for (const auto &[k, x] : *o) {
            auto it = s.properties.find(k);
            if (it == s.properties.end() || !valid(x, it->second, depth + 1))
                return false;
        }
        return true;
    }
    case K::union_value:
        for (const auto &alternative : s.variants)
            if (valid(v, alternative, depth + 1))
                return true;
        return false;
    }
    return false;
}
} // namespace
Result<void> validate_value(const JsonValue &v, const ValueSchema &s) {
    if (!valid(v, s, 0))
        return Result<void>::failure(
            ErrorEnvelope::make(ErrorCode::invalid_result, ErrorStage::result_validation));
    auto canonical = canonical_json(v);
    if (!canonical)
        return Result<void>::failure(
            ErrorEnvelope::make(ErrorCode::invalid_result, ErrorStage::result_validation));
    return Result<void>::success();
}
bool schema_assignable(const ValueSchema &a, const ValueSchema &b) {
    using K = ValueSchema::Kind;
    if (a.service_handle_type != b.service_handle_type)
        return false;
    if (a.kind == K::union_value) {
        for (const auto &v : a.variants)
            if (!schema_assignable(v, b))
                return false;
        return true;
    }
    if (b.kind == K::union_value) {
        for (const auto &v : b.variants)
            if (schema_assignable(a, v))
                return true;
        return false;
    }
    if (a.kind != b.kind && !(a.kind == K::integer && b.kind == K::number))
        return false;
    switch (a.kind) {
    case K::number:
    case K::integer:
        return a.minimum >= b.minimum && a.maximum <= b.maximum;
    case K::string:
        return a.max_bytes <= b.max_bytes;
    case K::array:
        if (a.max_items > b.max_items)
            return false;
        if (!a.tuple_items.empty()) {
            for (const auto &item : a.tuple_items)
                if (!b.items || !schema_assignable(item, *b.items))
                    return false;
            return true;
        }
        return a.items && b.items && schema_assignable(*a.items, *b.items);
    case K::object:
        for (const auto &r : b.required)
            if (!a.required.contains(r))
                return false;
        for (const auto &[k, v] : a.properties) {
            auto it = b.properties.find(k);
            if (it == b.properties.end() || !schema_assignable(v, it->second))
                return false;
        }
        return true;
    default:
        return true;
    }
}
namespace {
JsonValue export_schema(const ValueSchema &s, unsigned depth, std::size_t &nodes) {
    if (depth > 16 || ++nodes > 65536 || s.properties.size() > 4096 || s.max_bytes > 262144 ||
        s.max_items > 65536 || s.tuple_items.size() > 65536 || s.variants.size() > 256)
        reject();
    if (s.service_handle_type &&
        (s.service_handle_type->empty() || s.service_handle_type->size() > 128))
        reject();
    if ((s.kind == ValueSchema::Kind::integer || s.kind == ValueSchema::Kind::number) &&
        (!std::isfinite(s.minimum) || !std::isfinite(s.maximum) || s.minimum > s.maximum ||
         std::abs(s.minimum) > 9007199254740991.0 || std::abs(s.maximum) > 9007199254740991.0 ||
         (s.kind == ValueSchema::Kind::integer &&
          (std::floor(s.minimum) != s.minimum || std::floor(s.maximum) != s.maximum))))
        reject();
    for (const auto &key : s.required)
        if (!s.properties.contains(key))
            reject();
    using K = ValueSchema::Kind;
    JsonValue::Object out;
    switch (s.kind) {
    case K::null_value:
        out["type"] = "null";
        break;
    case K::boolean:
        out["type"] = "boolean";
        break;
    case K::integer:
    case K::number:
        out["type"] = s.kind == K::integer ? "integer" : "number";
        out["minimum"] = s.minimum;
        out["maximum"] = s.maximum;
        break;
    case K::string:
        out["type"] = "string";
        out["max_bytes"] = integer(s.max_bytes);
        break;
    case K::array:
        out["type"] = "array";
        out["max_items"] = integer(s.max_items);
        if (s.items)
            out["items"] = export_schema(*s.items, depth + 1, nodes);
        if (!s.tuple_items.empty()) {
            JsonValue::Array tuple;
            for (const auto &t : s.tuple_items)
                tuple.push_back(export_schema(t, depth + 1, nodes));
            out["tuple_items"] = std::move(tuple);
        }
        break;
    case K::object: {
        out["type"] = "object";
        out["additional_properties"] = false;
        JsonValue::Object props;
        for (const auto &[k, v] : s.properties)
            props[k] = export_schema(v, depth + 1, nodes);
        out["properties"] = std::move(props);
        JsonValue::Array req;
        for (const auto &r : s.required)
            req.emplace_back(r);
        out["required"] = std::move(req);
        break;
    }
    case K::union_value: {
        out["type"] = "union";
        JsonValue::Array variants;
        for (const auto &v : s.variants)
            variants.push_back(export_schema(v, depth + 1, nodes));
        out["variants"] = std::move(variants);
        break;
    }
    }
    if (s.service_handle_type)
        out["service_handle_type"] = *s.service_handle_type;
    return out;
}
} // namespace
JsonValue schema_export(const ValueSchema &s) {
    std::size_t nodes = 0;
    return export_schema(s, 0, nodes);
}
Result<JsonValue> resolve_binding(const Binding &b, const JsonValue::Object &inputs,
                                  const JsonValue::Object &outputs) {
    try {
        if (b.literal && !b.reference)
            return Result<JsonValue>::success(*b.literal);
        if (!b.reference || b.literal)
            reject(ErrorCode::invalid_reference);
        const auto &r = *b.reference;
        const auto &source = r.source == Reference::Source::input ? inputs : outputs;
        auto it = source.find(r.name);
        if (it == source.end())
            reject(ErrorCode::invalid_reference);
        const auto *v = path_value(it->second, r.path);
        if (!v)
            reject(ErrorCode::invalid_reference);
        return Result<JsonValue>::success(*v);
    } catch (...) {
        return Result<JsonValue>::failure(ErrorEnvelope::make(ErrorCode::invalid_reference));
    }
}
bool evaluate_acceptance(const AcceptanceExpression &e, const JsonValue &v) {
    using O = AcceptanceExpression::Op;
    switch (e.op) {
    case O::always:
        return true;
    case O::exists:
        return path_value(v, e.path) != nullptr;
    case O::eq: {
        auto *p = path_value(v, e.path);
        return p && e.value && *p == *e.value;
    }
    case O::all:
        if (e.args.empty())
            return false;
        for (const auto &a : e.args)
            if (!evaluate_acceptance(a, v))
                return false;
        return true;
    case O::any:
        for (const auto &a : e.args)
            if (evaluate_acceptance(a, v))
                return true;
        return false;
    case O::negate:
        return e.args.size() == 1 && !evaluate_acceptance(e.args[0], v);
    }
    return false;
}
} // namespace flamoris::runtime
