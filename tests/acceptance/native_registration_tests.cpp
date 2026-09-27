#include "catch_amalgamated.hpp"
#include "flamoris/runtime/native_registration.hpp"
using namespace flamoris::runtime;
TEST_CASE("C08 C10 native registration binds actual model processor tokenizer compute and memory "
          "before load") {
    NativeWorkerConfig config;
    config.artifact_sha256 = "5cd5a0bacb4cce1efd801fe2a4d7c1347467538ef6e393b37d955d2499ef74e2";
    auto pins = expected_native_pins(config);
    REQUIRE(pins);
    CapabilityContract capability(EffectSet::from_mask(1).value());
    capability.inference = true;
    capability.pausable = true;
    capability.cancellable = true;
    capability.native_pins = pins.value();
    ResourceVector admitted;
    admitted[ResourceKind::ram] = 222592;
    admitted[ResourceKind::execution] = 1;
    REQUIRE(validate_native_registration(config, capability, admitted));
    REQUIRE(config.registered_artifact
                .empty()); // Pure validation does not read a path or load a model.
    for (auto field : {"model", "processor", "tokenizer", "execution_profile", "compute"}) {
        auto changed = capability;
        changed.native_pins[field] += ":stale";
        REQUIRE_FALSE(validate_native_registration(config, changed, admitted));
    }
    --admitted[ResourceKind::ram];
    REQUIRE_FALSE(validate_native_registration(config, capability, admitted));
    config.opencl = true;
    config.compute_identity = "flamoris.opencl-fp32.v1:test:device:driver";
    capability.native_pins = expected_native_pins(config).value();
    admitted[ResourceKind::ram] = 2097152;
    REQUIRE_FALSE(validate_native_registration(config, capability, admitted));
    config.retained_context_bytes = 1048576;
    admitted[ResourceKind::device] = config.device_memory_bound;
    admitted[ResourceKind::transfer] = config.device_memory_bound;
    REQUIRE(validate_native_registration(config, capability, admitted));
}
