#include "flamoris/runtime/media_lowering.hpp"
#include <algorithm>
#include <functional>

namespace flamoris::runtime {
namespace {
bool media_digest(const std::string &value) {
    return value.size() == 71 && value.starts_with("sha256:") &&
           value.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
}
bool revision(const std::string &value) {
    return !value.empty() && value.size() <= 128 &&
           value.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._/-") ==
               std::string::npos;
}
bool occurrence(const std::string &value) {
    if (value != "root" && !value.starts_with("root/"))
        return false;
    return value.size() <= 256 && !value.ends_with('/') && value.find("//") == std::string::npos &&
           value.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_/-") ==
               std::string::npos;
}
Result<JsonValue> binding_json(const Binding &binding,
                               const std::map<std::string, std::string> &nodes) {
    if (binding.literal.has_value() == binding.reference.has_value())
        return Result<JsonValue>::failure(ErrorEnvelope::make(ErrorCode::invalid_reference));
    if (binding.literal)
        return Result<JsonValue>::success(JsonValue::Object{{"literal", *binding.literal}});
    const auto &ref = *binding.reference;
    std::string name = ref.name;
    if (ref.source == Reference::Source::node) {
        auto node = nodes.find(name);
        if (node == nodes.end())
            return Result<JsonValue>::failure(ErrorEnvelope::make(ErrorCode::invalid_reference));
        name = node->second;
    }
    JsonValue::Array path;
    for (const auto &part : ref.path) {
        if (const auto *key = std::get_if<std::string>(&part))
            path.emplace_back(*key);
        else {
            const auto index = std::get<std::uint64_t>(part);
            if (index > 9007199254740991ULL)
                return Result<JsonValue>::failure(
                    ErrorEnvelope::make(ErrorCode::invalid_reference));
            path.emplace_back(static_cast<double>(index));
        }
    }
    return Result<JsonValue>::success(JsonValue::Object{
        {"ref",
         JsonValue::Object{{"source", ref.source == Reference::Source::node ? "node" : "input"},
                           {"name", std::move(name)},
                           {"path", std::move(path)}}}});
}
JsonValue identity_json(const MediaIdentity &id) {
    return JsonValue::Object{{"root_digest", id.root_digest},
                             {"closure_digest", id.closure_digest},
                             {"structural_digest", id.structural_digest},
                             {"invocation_digest", id.invocation_digest},
                             {"evidence_digest", id.evidence_digest},
                             {"generation_compiler_revision", id.generation_compiler_revision},
                             {"lowering_revision", id.lowering_revision}};
}
} // namespace
Result<LoweredMediaSubmission>
lower_media_submission(const MediaLoweringRequest &request, const CapabilitySnapshot &registry,
                       const std::map<std::string, MediaCapabilityRegistration> &approved,
                       CompilerProfile profile) {
    auto fail = [](ErrorCode code) {
        return Result<LoweredMediaSubmission>::failure(ErrorEnvelope::make(code));
    };
    try {
        const auto &id = request.identity;
        if (!media_digest(id.root_digest) || !media_digest(id.closure_digest) ||
            !media_digest(id.structural_digest) || !media_digest(id.invocation_digest) ||
            !media_digest(id.evidence_digest) || !revision(id.generation_compiler_revision) ||
            !revision(id.lowering_revision) || request.operations.empty() ||
            request.operations.size() > 128 || request.input_schemas.size() > 64 ||
            request.outputs.empty() || request.outputs.size() > 64 ||
            request.input_values.size() > 64)
            return fail(ErrorCode::invalid_workflow);
        LoweredMediaSubmission result;
        result.identity = id;
        std::map<std::string, const MediaOperation *> operations;
        for (const auto &op : request.operations)
            if (!occurrence(op.occurrence) || !operations.emplace(op.occurrence, &op).second)
                return fail(ErrorCode::invalid_workflow);
        for (const auto &[path, op] : operations) {
            (void)op;
            result.occurrence_nodes.emplace(
                path, "media_" + std::to_string(result.occurrence_nodes.size()));
        }
        JsonValue::Array nodes, edges;
        std::vector<EffectSet> effects;
        std::map<std::string, std::set<std::string>> dependencies;
        for (const auto &[path, operation] : operations) {
            const auto &op = *operation;
            if (op.inputs.size() > 64 || op.dependencies.size() > 128)
                return fail(ErrorCode::invalid_workflow);
            auto registration = approved.find(op.pin.identifier);
            if (registration == approved.end() ||
                registration->second.purpose != MediaOperationPurpose::internal_provider)
                return fail(ErrorCode::permission_denied);
            if (registration->second.pin != op.pin)
                return fail(ErrorCode::plan_stale);
            auto capability = registry.capabilities.find(op.pin.identifier);
            if (capability == registry.capabilities.end())
                return fail(ErrorCode::unknown_capability);
            const auto &contract = capability->second;
            auto pin = fingerprint_capability(contract);
            if (!pin || pin.value() != op.pin)
                return fail(ErrorCode::plan_stale);
            if (!contract.available)
                return fail(ErrorCode::capability_unavailable);
            // Initial bridge operations have no native/child/control authority.
            if (contract.inference || op.pin.identifier.starts_with("control.") ||
                contract.resource_contract_digest.empty())
                return fail(ErrorCode::invalid_workflow);
            if (op.timeout_ms == 0 || op.timeout_ms > request.limits.timeout_ms ||
                op.timeout_ms > contract.max_timeout_ms)
                return fail(ErrorCode::budget_exceeded);
            effects.push_back(contract.effects);
            JsonValue::Object inputs;
            auto &parents = dependencies[path];
            for (const auto &[name, binding] : op.inputs) {
                auto lowered = binding_json(binding, result.occurrence_nodes);
                if (!lowered)
                    return Result<LoweredMediaSubmission>::failure(lowered.error());
                inputs.emplace(name, std::move(lowered.value()));
                if (binding.reference && binding.reference->source == Reference::Source::node)
                    parents.insert(binding.reference->name);
            }
            std::set<std::string> explicit_dependencies;
            for (const auto &parent : op.dependencies) {
                if (!operations.contains(parent) || !explicit_dependencies.insert(parent).second)
                    return fail(ErrorCode::invalid_reference);
                parents.insert(parent);
                edges.emplace_back(JsonValue::Object{{"from", result.occurrence_nodes.at(parent)},
                                                     {"to", result.occurrence_nodes.at(path)}});
            }
            nodes.emplace_back(
                JsonValue::Object{{"id", result.occurrence_nodes.at(path)},
                                  {"type", op.pin.identifier},
                                  {"with", std::move(inputs)},
                                  {"timeout_ms", static_cast<double>(op.timeout_ms)}});
        }
        auto aggregate = EffectSet::aggregate(effects);
        if (!aggregate || aggregate.value() != request.effects)
            return fail(ErrorCode::invalid_workflow);
        JsonValue::Object outputs;
        std::set<std::string> reachable;
        std::function<void(const std::string &)> visit = [&](const std::string &path) {
            if (reachable.insert(path).second)
                for (const auto &parent : dependencies.at(path))
                    visit(parent);
        };
        for (const auto &[name, binding] : request.outputs) {
            auto lowered = binding_json(binding, result.occurrence_nodes);
            if (!lowered)
                return Result<LoweredMediaSubmission>::failure(lowered.error());
            // Public media results must come from explicit stage outputs.
            if (!binding.reference || binding.reference->source != Reference::Source::node)
                return fail(ErrorCode::invalid_reference);
            visit(binding.reference->name);
            outputs.emplace(name, std::move(lowered.value()));
        }
        if (reachable.size() != operations.size())
            return fail(ErrorCode::invalid_workflow);
        JsonValue::Object schemas;
        for (const auto &[name, schema] : request.input_schemas)
            schemas.emplace(name, schema_export(schema));
        JsonValue::Object limits;
        // Export every ceiling. The existing compiler rejects values outside its
        // current profile and counts the root Job/attempts/mandatory events too.
        const auto &l = request.limits;
        const std::map<std::string, std::uint64_t> ceilings{
            {"timeout_ms", l.timeout_ms},
            {"max_parallelism", l.max_parallelism},
            {"max_jobs", l.max_jobs},
            {"max_attempts", l.max_attempts},
            {"max_dynamic_proposals", l.max_dynamic_proposals},
            {"max_child_depth", l.max_child_depth},
            {"max_output_bytes", l.max_output_bytes},
            {"max_control_steps", l.max_control_steps},
            {"max_suspensions", l.max_suspensions},
            {"max_control_commands", l.max_control_commands},
            {"max_resource_operations", l.max_resource_operations},
            {"max_event_bytes", l.max_event_bytes},
            {"max_trace_bytes", l.max_trace_bytes}};
        for (const auto &[name, value] : ceilings) {
            if (value > 9007199254740991ULL)
                return fail(ErrorCode::budget_exceeded);
            limits.emplace(name, static_cast<double>(value));
        }
        JsonValue wire = JsonValue::Object{
            {"schema_version", "flamoris.submit/1"},
            {"kind", "workflow"},
            {"workflow", JsonValue::Object{{"schema_version", "flamoris.workflow/0.1"},
                                           {"workflow", JsonValue::Object{{"id", "media_root"}}},
                                           {"inputs", std::move(schemas)},
                                           {"nodes", std::move(nodes)},
                                           {"edges", std::move(edges)},
                                           {"outputs", std::move(outputs)},
                                           {"limits", std::move(limits)}}},
            {"input_values", request.input_values}};
        auto canonical = canonical_json(wire);
        if (!canonical)
            return Result<LoweredMediaSubmission>::failure(canonical.error());
        auto compiled = Compiler(profile).compile_submission(canonical.value(), registry);
        if (!compiled)
            return Result<LoweredMediaSubmission>::failure(compiled.error());
        JsonValue::Object paths;
        for (const auto &[path, node] : result.occurrence_nodes)
            paths.emplace(path, node);
        auto relation =
            domain_digest("flamoris.media-runtime-lowering/1\n",
                          JsonValue::Object{{"media", identity_json(id)},
                                            {"occurrences", std::move(paths)},
                                            {"runtime_plan", compiled.value().plan->fingerprint},
                                            {"runtime_request", compiled.value().request_digest}});
        if (!relation)
            return Result<LoweredMediaSubmission>::failure(relation.error());
        result.canonical_submission = std::move(canonical.value());
        result.relation_digest = std::move(relation.value());
        result.submission = std::move(compiled.value());
        return Result<LoweredMediaSubmission>::success(std::move(result));
    } catch (...) {
        return fail(ErrorCode::invalid_workflow);
    }
}
} // namespace flamoris::runtime
