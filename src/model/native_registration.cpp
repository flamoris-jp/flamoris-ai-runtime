#include "flamoris/runtime/native_registration.hpp"

namespace flamoris::runtime {
namespace {
constexpr std::string_view fixture_digest =
    "5cd5a0bacb4cce1efd801fe2a4d7c1347467538ef6e393b37d955d2499ef74e2";
ErrorEnvelope invalid(ErrorCode code = ErrorCode::invalid_request) {
    return ErrorEnvelope::make(code, ErrorStage::admission, ExternalOutcome::not_applicable,
                               RetryDisposition::prohibited, ErrorReason::version_mismatch);
}
} // namespace
Result<std::map<std::string, std::string>> expected_native_pins(const NativeWorkerConfig &config) {
    if (config.artifact_sha256 != fixture_digest || config.processor != ProcessorDefinition{} ||
        !validate_tokenizer(config.tokenizer) || config.compute_identity.empty() ||
        config.compute_identity.size() > 12288 ||
        (!config.opencl && config.compute_identity != "flamoris.cpu-fp32.v1") ||
        (config.opencl && !config.compute_identity.starts_with("flamoris.opencl-fp32.v1:")))
        return Result<std::map<std::string, std::string>>::failure(
            invalid(ErrorCode::unsupported_model));
    return Result<std::map<std::string, std::string>>::success(
        {{"model", "flamoris.tiny-causal.v1:" + config.artifact_sha256},
         {"processor", config.processor.identity + ":" + config.processor.fingerprint},
         {"tokenizer", config.tokenizer.identity + ":" + config.tokenizer.fingerprint},
         {"execution_profile", "flamoris.tiny-causal-fp32.v1"},
         {"compute", config.compute_identity}});
}
Result<void> validate_native_registration(const NativeWorkerConfig &config,
                                          const CapabilityContract &capability,
                                          const ResourceVector &requirement) {
    auto pins = expected_native_pins(config);
    if (!pins)
        return Result<void>::failure(pins.error());
    // Runtime residency pooling is a distinct owner contract; do not double-charge
    // externally shared pointers as independently owned model allocations.
    if (config.shared_model)
        return Result<void>::failure(invalid(ErrorCode::capability_unavailable));
    if (!capability.inference || !capability.pausable || !capability.cancellable ||
        capability.native_pins != pins.value())
        return Result<void>::failure(invalid(ErrorCode::plan_stale));
    constexpr std::uint64_t model = 25984, state = 131072, scratch = 65536;
    if (config.retained_context_bytes > 1073741824 ||
        (!config.opencl && config.retained_context_bytes != 0) ||
        (config.opencl && (!config.retained_context_bytes || !config.device_memory_bound ||
                           config.device_memory_bound > 16777216 ||
                           requirement[ResourceKind::device] < config.device_memory_bound ||
                           requirement[ResourceKind::transfer] < config.device_memory_bound)) ||
        requirement[ResourceKind::ram] < model + state + scratch + config.retained_context_bytes ||
        requirement[ResourceKind::execution] < 1)
        return Result<void>::failure(invalid(ErrorCode::resource_unavailable));
    return Result<void>::success();
}
} // namespace flamoris::runtime
