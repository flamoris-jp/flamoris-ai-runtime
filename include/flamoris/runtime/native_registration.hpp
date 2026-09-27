#pragma once
#include "flamoris/runtime/compiler.hpp"
#include "flamoris/runtime/inference.hpp"
#include "flamoris/runtime/resources.hpp"

namespace flamoris::runtime {
// Pure registration validation: no file reads, native allocation or device probing.
Result<std::map<std::string, std::string>> expected_native_pins(const NativeWorkerConfig &);
Result<void> validate_native_registration(const NativeWorkerConfig &, const CapabilityContract &,
                                          const ResourceVector &admitted_requirement);
} // namespace flamoris::runtime
