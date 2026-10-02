#pragma once
#include "flamoris/runtime/compiler.hpp"

namespace flamoris::runtime {
// Trusted adapter configuration classifies permitted operations. Portable media
// data cannot register a capability or turn root admission into an internal stage.
enum class MediaOperationPurpose { internal_provider, public_root_admission };
struct MediaCapabilityRegistration {
    CapabilityPin pin;
    MediaOperationPurpose purpose{MediaOperationPurpose::internal_provider};
};
struct MediaIdentity {
    std::string root_digest, closure_digest, structural_digest, invocation_digest, evidence_digest;
    std::string generation_compiler_revision, lowering_revision;
};
struct MediaOperation {
    std::string occurrence;
    CapabilityPin pin;
    std::map<std::string, Binding> inputs;
    std::vector<std::string> dependencies;
    std::uint64_t timeout_ms{0};
};
// Already domain-validated operations supplied by the owning Generation adapter.
// Node references use occurrence paths, not generated Runtime node identifiers.
// This is an in-process compile boundary, not a new public media/wire schema.
struct MediaLoweringRequest {
    MediaIdentity identity;
    std::vector<MediaOperation> operations;
    std::map<std::string, ValueSchema> input_schemas;
    JsonValue::Object input_values;
    std::map<std::string, Binding> outputs;
    RunLimits limits;
    EffectSet effects{EffectSet::from_mask(1).value()};
};
struct LoweredMediaSubmission {
    CompiledSubmission submission;
    std::string canonical_submission, relation_digest;
    MediaIdentity identity;
    std::map<std::string, std::string> occurrence_nodes;
};
// Pure compilation: no admission, provider call, handle materialization or host
// grant. Runtime still checks current authorization/pins/resources at execution.
Result<LoweredMediaSubmission>
lower_media_submission(const MediaLoweringRequest &, const CapabilitySnapshot &,
                       const std::map<std::string, MediaCapabilityRegistration> &approved,
                       CompilerProfile profile = {});
} // namespace flamoris::runtime
