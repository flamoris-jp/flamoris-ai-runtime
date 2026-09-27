#pragma once
#include "flamoris/runtime/effects.hpp"
#include "flamoris/runtime/result.hpp"
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace flamoris::runtime {
// Owned boundary values. Parser implementation and third-party JSON types stay private.
struct JsonValue {
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue>;
    std::variant<std::nullptr_t, bool, double, std::string, Array, Object> data{nullptr};
    JsonValue() = default;
    JsonValue(std::nullptr_t) : data(nullptr) {}
    JsonValue(bool v) : data(v) {}
    JsonValue(double v) : data(v), exact_integer(std::isfinite(v) && std::floor(v) == v) {}
    JsonValue(std::string v) : data(std::move(v)) {}
    JsonValue(const char *v) : data(std::string(v)) {}
    JsonValue(Array v) : data(std::move(v)) {}
    JsonValue(Object v) : data(std::move(v)) {}
    // Parsing provenance for integer/count schemas; binary64 value equality stays semantic.
    bool exact_integer{true};
    bool operator==(const JsonValue &other) const { return data == other.data; }
};
struct JsonBounds {
    std::size_t max_bytes{1048576}, max_depth{32}, max_string_bytes{262144};
    std::size_t max_scalar_bytes{1048576}, max_key_bytes{256}, max_values{65536};
};
Result<JsonValue> parse_bounded_json(std::string_view, JsonBounds = {});
Result<JsonValue> parse_bounded_json_value(std::string_view, JsonBounds = {});
Result<std::string> canonical_json(const JsonValue &);
Result<std::string> domain_digest(std::string_view domain, const JsonValue &);

struct ValueSchema {
    enum class Kind { null_value, boolean, integer, number, string, array, object, union_value };
    Kind kind{Kind::null_value};
    double minimum{0}, maximum{0};
    std::uint64_t max_bytes{0}, max_items{0};
    std::shared_ptr<const ValueSchema> items;
    std::vector<ValueSchema> tuple_items;
    std::map<std::string, ValueSchema> properties;
    std::set<std::string> required;
    std::vector<ValueSchema> variants;
    std::optional<std::string> service_handle_type;
    bool from_literal{false}; // compiler provenance; never a caller schema field
};
Result<ValueSchema> parse_value_schema(const JsonValue &);
Result<void> validate_value(const JsonValue &, const ValueSchema &);
bool schema_assignable(const ValueSchema &source, const ValueSchema &target);
JsonValue schema_export(const ValueSchema &);

struct RunLimits {
    std::uint64_t timeout_ms{60000}, max_parallelism{4}, max_jobs{64}, max_attempts{128};
    std::uint64_t max_dynamic_proposals{16}, max_child_depth{8}, max_output_bytes{1048576};
    std::uint64_t max_control_steps{1024}, max_suspensions{16}, max_control_commands{64};
    std::uint64_t max_resource_operations{64}, max_event_bytes{67108864}, max_trace_bytes{1048576};
};
struct CapabilityPin {
    std::string identifier, fingerprint;
    bool operator==(const CapabilityPin &) const = default;
};
struct ValidatedHandleAccess {
    std::string owner, object_scope;
    std::uint64_t expires_at_ms{0};
};
struct CapabilityContract {
    explicit CapabilityContract(EffectSet declared_effects) : effects(declared_effects) {}
    std::string identifier, version, adapter_revision;
    ValueSchema input_schema, output_schema;
    EffectSet effects;
    bool available{true}, inference{false}, retry_permitted{false}, provider_deduplication{false};
    bool cancellable{false}, pausable{false};
    std::uint64_t max_attempts{1}, max_output_bytes{1048576}, max_timeout_ms{60000};
    std::uint64_t resource_units{1};
    // Runtime fills this from its immutable full resource/native registration.
    // Empty is permitted only for standalone compiler-only registrations.
    std::string resource_contract_digest;
    // Trusted registration proofs, never workflow-authored declarations.
    std::optional<std::string> disjoint_scope;
    std::map<std::string, std::string> native_pins;
    std::vector<std::string> object_scope_fields;
    std::string handle_validator_revision;
    // Trusted local bounded validation only: no hidden I/O or object materialization.
    std::function<Result<ValidatedHandleAccess>(const JsonValue &, std::string_view, std::uint64_t)>
        handle_validator;
};
struct ChildEnvelope {
    std::string identifier, revision;
    std::vector<CapabilityPin> capabilities;
    std::uint64_t max_children{0}, max_depth{0}, max_attempts{0}, max_output_bytes{0};
};
struct CapabilitySnapshot {
    std::map<std::string, CapabilityContract> capabilities;
    std::map<std::string, ChildEnvelope> child_policies;
};
Result<CapabilityPin> fingerprint_capability(const CapabilityContract &);

using PathComponent = std::variant<std::string, std::uint64_t>;
struct Reference {
    enum class Source { input, node };
    Source source{Source::input};
    std::string name;
    std::vector<PathComponent> path;
};
struct Binding {
    std::optional<JsonValue> literal;
    std::optional<Reference> reference;
};
struct AcceptanceExpression {
    enum class Op { always, exists, eq, all, any, negate };
    Op op{Op::always};
    std::vector<PathComponent> path;
    std::optional<JsonValue> value;
    std::vector<AcceptanceExpression> args;
};
bool evaluate_acceptance(const AcceptanceExpression &, const JsonValue &);
Result<JsonValue> resolve_binding(const Binding &, const JsonValue::Object &inputs,
                                  const JsonValue::Object &node_outputs);
struct PlanStep {
    std::string id, type, structural_path;
    std::map<std::string, Binding> inputs;
    std::vector<std::string> dependencies;
    std::vector<PlanStep> members;
    ValueSchema output_schema;
    EffectSet effects{EffectSet::from_mask(1).value()};
    std::optional<CapabilityPin> capability_pin;
    std::optional<ChildEnvelope> child_envelope;
    std::optional<AcceptanceExpression> acceptance;
    std::string group_policy;
    bool pause_supported{false};
    std::uint64_t timeout_ms{0}, max_attempts{1}, backoff_ms{0};
};
struct ResultOutputContract {
    ValueSchema schema;
    std::optional<CapabilityPin> handle_validator;
};
struct ExecutionPlan {
    std::string schema_revision{"flamoris.plan/1"}, compiler_revision{"flamoris.compiler/1"};
    std::string profile_revision, canonical_export, fingerprint;
    std::vector<PlanStep> steps;
    std::map<std::string, ValueSchema> inputs;
    std::map<std::string, Binding> outputs;
    // Schema and trusted validator provenance of the values actually exposed by result().
    std::map<std::string, ResultOutputContract> result_outputs;
    std::vector<CapabilityPin> pins;
    RunLimits limits;
    EffectSet effects{EffectSet::from_mask(1).value()};
    bool single_root_inference{false};
    std::uint64_t static_jobs{0}, static_attempts{0}, mandatory_event_bytes{0};
};
struct SubmissionIdentity {
    std::string request_kind, request_digest;
    std::optional<std::string> idempotency_key;
};
Result<SubmissionIdentity> validate_submission_identity(std::string_view json);
struct CompiledSubmission {
    std::string request_kind, request_digest;
    std::optional<std::string> idempotency_key;
    JsonValue::Object input_values;
    std::shared_ptr<const ExecutionPlan> plan;
};
struct CompilerProfile {
    std::string revision{"flamoris.profile/1"};
    RunLimits limits;
    JsonBounds json_bounds;
};
class Compiler {
  public:
    explicit Compiler(CompilerProfile profile = {}) : profile_(std::move(profile)) {}
    Result<CompiledSubmission> compile_submission(std::string_view json,
                                                  const CapabilitySnapshot &) const;

  private:
    CompilerProfile profile_;
};
// Pure compilation only. The owning Run charges proposals before this call and
// atomically reserves admitted child/attempt/resource obligations afterwards.
// child_depth and max_child_depth/max_depth are absolute depths within the original Run.
Result<CompiledSubmission> compile_child_fragment(std::string_view json, const CapabilitySnapshot &,
                                                  const ChildEnvelope &, const RunLimits &remaining,
                                                  std::uint64_t child_depth);
Result<void> verify_plan_pins(const ExecutionPlan &, const CapabilitySnapshot &,
                              bool require_available = true);
} // namespace flamoris::runtime
