#pragma once
#include "flamoris/runtime/result.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace flamoris::runtime {
// Compute objects own no Job lifecycle or inference loop. One owner serializes calls.
struct ComputeDevice {
    std::string identity;
    std::string name;
    std::string driver;
    bool accelerator{false};
};
struct ComputeReceipt {
    std::uint64_t completed_operations{0};
    std::size_t live_device_bytes{0};
    std::size_t peak_device_bytes{0};
    bool quiescent{true};
};
class ComputeImplementation {
  public:
    virtual ~ComputeImplementation() = default;
    virtual const ComputeDevice &device() const noexcept = 0;
    virtual Result<std::vector<float>> matvec(std::span<const float> matrix, std::size_t rows,
                                              std::size_t columns,
                                              std::span<const float> input) = 0;
    virtual Result<std::vector<float>> attention(std::span<const float> query,
                                                 std::span<const float> keys,
                                                 std::span<const float> values,
                                                 std::size_t positions, std::size_t width) = 0;
    virtual ComputeReceipt receipt() const noexcept = 0;
    virtual Result<void> synchronize() = 0;
    // Successful close proves retained native objects have been physically released.
    // Implementations without retained external objects need only synchronize.
    virtual Result<void> close() { return synchronize(); }
};
std::unique_ptr<ComputeImplementation> make_cpu_compute();
Result<std::vector<ComputeDevice>> enumerate_opencl_devices();
// A failed initialization can still own native objects. The caller must retain
// owner, and its admitted resources, until close() acknowledges physical release.
struct ComputePreparation {
    std::unique_ptr<ComputeImplementation> owner;
    std::optional<ErrorEnvelope> error;
};
ComputePreparation prepare_opencl_compute(std::size_t device_index,
                                          std::size_t max_device_bytes = 1048576);
} // namespace flamoris::runtime
