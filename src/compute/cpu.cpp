#include "flamoris/runtime/compute.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace flamoris::runtime {
namespace {
template <class T> Result<T> invalid() {
    return Result<T>::failure(
        ErrorEnvelope::make(ErrorCode::invalid_request, ErrorStage::execution));
}
class CpuCompute final : public ComputeImplementation {
  public:
    const ComputeDevice &device() const noexcept override { return device_; }
    Result<std::vector<float>> matvec(std::span<const float> matrix, std::size_t rows,
                                      std::size_t columns, std::span<const float> input) override {
        if (!rows || !columns || rows > 1024 || columns > 1024 || matrix.size() != rows * columns ||
            input.size() != columns)
            return invalid<std::vector<float>>();
        std::vector<float> output(rows, 0.0F);
        for (std::size_t r = 0; r < rows; ++r) {
            for (std::size_t c = 0; c < columns; ++c)
                output[r] += matrix[r * columns + c] * input[c];
            if (!std::isfinite(output[r]))
                return invalid<std::vector<float>>();
        }
        ++completed_;
        return Result<std::vector<float>>::success(std::move(output));
    }
    Result<std::vector<float>> attention(std::span<const float> query, std::span<const float> keys,
                                         std::span<const float> values, std::size_t positions,
                                         std::size_t width) override {
        if (!positions || positions > 256 || !width || width > 128 || query.size() != width ||
            keys.size() != positions * width || values.size() != keys.size())
            return invalid<std::vector<float>>();
        std::vector<float> scores(positions, 0.0F), output(width, 0.0F);
        const float scale = 1.0F / std::sqrt(static_cast<float>(width));
        for (std::size_t p = 0; p < positions; ++p) {
            for (std::size_t d = 0; d < width; ++d)
                scores[p] += query[d] * keys[p * width + d];
            scores[p] *= scale;
        }
        const float maximum = *std::max_element(scores.begin(), scores.end());
        float total = 0.0F;
        for (auto &score : scores) {
            score = std::exp(score - maximum);
            total += score;
        }
        if (!std::isfinite(total) || total <= 0)
            return invalid<std::vector<float>>();
        for (std::size_t p = 0; p < positions; ++p)
            for (std::size_t d = 0; d < width; ++d)
                output[d] += scores[p] / total * values[p * width + d];
        for (auto v : output)
            if (!std::isfinite(v))
                return invalid<std::vector<float>>();
        ++completed_;
        return Result<std::vector<float>>::success(std::move(output));
    }
    ComputeReceipt receipt() const noexcept override { return {completed_, 0, 0, true}; }
    Result<void> synchronize() override { return Result<void>::success(); }

  private:
    ComputeDevice device_{"flamoris.cpu-fp32.v1", "native scalar CPU", "C++20", false};
    std::uint64_t completed_{0};
};
} // namespace
std::unique_ptr<ComputeImplementation> make_cpu_compute() { return std::make_unique<CpuCompute>(); }
} // namespace flamoris::runtime
