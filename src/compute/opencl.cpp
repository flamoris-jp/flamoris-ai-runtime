#include "flamoris/runtime/compute.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#ifdef FLAMORIS_ENABLE_OPENCL
#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>
#endif

namespace flamoris::runtime {
namespace {
template <class T> Result<T> unavailable() {
    return Result<T>::failure(
        ErrorEnvelope::make(ErrorCode::native_compute_unavailable, ErrorStage::execution));
}
#ifdef FLAMORIS_ENABLE_OPENCL
constexpr char kernels[] = R"CLC(
__kernel void matvec(__global const float* matrix, __global const float* input,
                     __global float* output, uint columns) {
    uint row = get_global_id(0);
    float total = 0.0f;
    for (uint c = 0; c < columns; ++c) total += matrix[row * columns + c] * input[c];
    output[row] = total;
}
__kernel void attention(__global const float* query, __global const float* keys,
    __global const float* values, __global float* output, uint positions, uint width) {
    uint channel = get_global_id(0);
    float maximum = -INFINITY;
    float scale = 1.0f / sqrt((float)width);
    for (uint p = 0; p < positions; ++p) {
        float score = 0.0f;
        for (uint d = 0; d < width; ++d) score += query[d] * keys[p * width + d];
        maximum = fmax(maximum, score * scale);
    }
    float denominator = 0.0f, numerator = 0.0f;
    for (uint p = 0; p < positions; ++p) {
        float score = 0.0f;
        for (uint d = 0; d < width; ++d) score += query[d] * keys[p * width + d];
        float probability = exp(score * scale - maximum);
        denominator += probability;
        numerator += probability * values[p * width + channel];
    }
    output[channel] = numerator / denominator;
}
)CLC";
std::string device_text(cl_device_id device, cl_device_info parameter) {
    std::size_t bytes = 0;
    if (clGetDeviceInfo(device, parameter, 0, nullptr, &bytes) != CL_SUCCESS || !bytes ||
        bytes > 4096)
        return {};
    std::string result(bytes, '\0');
    if (clGetDeviceInfo(device, parameter, bytes, result.data(), nullptr) != CL_SUCCESS)
        return {};
    while (!result.empty() && result.back() == '\0')
        result.pop_back();
    return result;
}
struct Discovered {
    ComputeDevice info;
    cl_device_id id;
};
Result<std::vector<Discovered>> discover() {
    cl_uint count = 0;
    if (clGetPlatformIDs(0, nullptr, &count) != CL_SUCCESS || !count || count > 64)
        return unavailable<std::vector<Discovered>>();
    std::vector<cl_platform_id> platforms(count);
    if (clGetPlatformIDs(count, platforms.data(), nullptr) != CL_SUCCESS)
        return unavailable<std::vector<Discovered>>();
    std::vector<Discovered> result;
    for (auto platform : platforms) {
        cl_uint devices = 0;
        auto status = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 0, nullptr, &devices);
        if (status == CL_DEVICE_NOT_FOUND)
            continue;
        if (status != CL_SUCCESS || devices > 64)
            return unavailable<std::vector<Discovered>>();
        std::vector<cl_device_id> ids(devices);
        if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, devices, ids.data(), nullptr) !=
            CL_SUCCESS)
            return unavailable<std::vector<Discovered>>();
        for (auto id : ids) {
            cl_device_type type = 0;
            cl_device_fp_config fp = 0;
            if (clGetDeviceInfo(id, CL_DEVICE_TYPE, sizeof(type), &type, nullptr) != CL_SUCCESS ||
                clGetDeviceInfo(id, CL_DEVICE_SINGLE_FP_CONFIG, sizeof(fp), &fp, nullptr) !=
                    CL_SUCCESS ||
                fp == 0)
                continue;
            auto name = device_text(id, CL_DEVICE_NAME),
                 driver = device_text(id, CL_DRIVER_VERSION);
            auto vendor = device_text(id, CL_DEVICE_VENDOR);
            if (name.empty() || driver.empty() || vendor.empty())
                return unavailable<std::vector<Discovered>>();
            result.push_back(
                {{"flamoris.opencl-fp32.v1:" + vendor + ":" + name + ":" + driver, name, driver,
                  (type & (CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_ACCELERATOR)) != 0},
                 id});
        }
    }
    return Result<std::vector<Discovered>>::success(std::move(result));
}
class OpenClCompute final : public ComputeImplementation {
  public:
    static Result<std::unique_ptr<ComputeImplementation>> create(const Discovered &device,
                                                                 std::size_t bound) {
        auto native = std::unique_ptr<OpenClCompute>(new OpenClCompute);
        native->device_ = device.info;
        native->bound_ = bound;
        native->buffers_.reserve(4);
        cl_int status = CL_SUCCESS;
        native->context_ = clCreateContext(nullptr, 1, &device.id, nullptr, nullptr, &status);
        if (status != CL_SUCCESS || !native->context_)
            return unavailable<std::unique_ptr<ComputeImplementation>>();
        native->queue_ = clCreateCommandQueue(native->context_, device.id, 0, &status);
        if (status != CL_SUCCESS || !native->queue_)
            return unavailable<std::unique_ptr<ComputeImplementation>>();
        const char *source = kernels;
        native->program_ =
            clCreateProgramWithSource(native->context_, 1, &source, nullptr, &status);
        if (status != CL_SUCCESS || !native->program_ ||
            clBuildProgram(native->program_, 1, &device.id, "-cl-std=CL1.2 -cl-opt-disable",
                           nullptr, nullptr) != CL_SUCCESS)
            return unavailable<std::unique_ptr<ComputeImplementation>>();
        native->matvec_ = clCreateKernel(native->program_, "matvec", &status);
        if (status != CL_SUCCESS || !native->matvec_)
            return unavailable<std::unique_ptr<ComputeImplementation>>();
        native->attention_ = clCreateKernel(native->program_, "attention", &status);
        if (status != CL_SUCCESS || !native->attention_)
            return unavailable<std::unique_ptr<ComputeImplementation>>();
        return Result<std::unique_ptr<ComputeImplementation>>::success(std::move(native));
    }
    ~OpenClCompute() override {
        // Objects are synchronous. If a driver cannot acknowledge completion, do not
        // free native handles still potentially in use; the worker must be retained/contained.
        if (!quiescent_)
            return;
        if (!clear_buffers())
            return;
        if (attention_)
            clReleaseKernel(attention_);
        if (matvec_)
            clReleaseKernel(matvec_);
        if (program_)
            clReleaseProgram(program_);
        if (queue_)
            clReleaseCommandQueue(queue_);
        if (context_)
            clReleaseContext(context_);
    }
    const ComputeDevice &device() const noexcept override { return device_; }
    Result<std::vector<float>> matvec(std::span<const float> matrix, std::size_t rows,
                                      std::size_t columns, std::span<const float> input) override {
        if (!rows || rows > 1024 || !columns || columns > 1024 || matrix.size() != rows * columns ||
            input.size() != columns)
            return invalid();
        const auto bytes = (matrix.size() + input.size() + rows) * sizeof(float);
        if (bytes > bound_ || !quiescent_ || !buffers_.empty())
            return invalid();
        auto a = buffer(matrix), b = buffer(input), out = output_buffer(rows);
        if (!a || !b || !out) {
            clear_buffers();
            return unavailable<std::vector<float>>();
        }
        const cl_uint cols = static_cast<cl_uint>(columns);
        if (clSetKernelArg(matvec_, 0, sizeof(a), &a) != CL_SUCCESS ||
            clSetKernelArg(matvec_, 1, sizeof(b), &b) != CL_SUCCESS ||
            clSetKernelArg(matvec_, 2, sizeof(out), &out) != CL_SUCCESS ||
            clSetKernelArg(matvec_, 3, sizeof(cols), &cols) != CL_SUCCESS) {
            clear_buffers();
            return unavailable<std::vector<float>>();
        }
        return execute(matvec_, rows, out);
    }
    Result<std::vector<float>> attention(std::span<const float> query, std::span<const float> keys,
                                         std::span<const float> values, std::size_t positions,
                                         std::size_t width) override {
        if (!positions || positions > 256 || !width || width > 128 || query.size() != width ||
            keys.size() != positions * width || values.size() != keys.size())
            return invalid();
        const auto bytes = (query.size() + keys.size() + values.size() + width) * sizeof(float);
        if (bytes > bound_ || !quiescent_ || !buffers_.empty())
            return invalid();
        auto q = buffer(query), k = buffer(keys), v = buffer(values), out = output_buffer(width);
        if (!q || !k || !v || !out) {
            clear_buffers();
            return unavailable<std::vector<float>>();
        }
        const cl_uint p = static_cast<cl_uint>(positions), w = static_cast<cl_uint>(width);
        if (clSetKernelArg(attention_, 0, sizeof(q), &q) != CL_SUCCESS ||
            clSetKernelArg(attention_, 1, sizeof(k), &k) != CL_SUCCESS ||
            clSetKernelArg(attention_, 2, sizeof(v), &v) != CL_SUCCESS ||
            clSetKernelArg(attention_, 3, sizeof(out), &out) != CL_SUCCESS ||
            clSetKernelArg(attention_, 4, sizeof(p), &p) != CL_SUCCESS ||
            clSetKernelArg(attention_, 5, sizeof(w), &w) != CL_SUCCESS) {
            clear_buffers();
            return unavailable<std::vector<float>>();
        }
        return execute(attention_, width, out);
    }
    ComputeReceipt receipt() const noexcept override {
        return {completed_, live_, peak_, quiescent_};
    }
    Result<void> synchronize() override {
        if (!queue_ || clFinish(queue_) != CL_SUCCESS)
            return Result<void>::failure(
                ErrorEnvelope::make(ErrorCode::cleanup_timeout, ErrorStage::cleanup));
        quiescent_ = true;
        if (!clear_buffers())
            return Result<void>::failure(
                ErrorEnvelope::make(ErrorCode::cleanup_failed, ErrorStage::cleanup));
        return Result<void>::success();
    }

  private:
    Result<std::vector<float>> invalid() {
        return Result<std::vector<float>>::failure(
            ErrorEnvelope::make(ErrorCode::resource_unavailable, ErrorStage::execution));
    }
    cl_mem buffer(std::span<const float> input) {
        if (!std::all_of(input.begin(), input.end(),
                         [](float value) { return std::isfinite(value); }))
            return nullptr;
        cl_int status;
        auto mem = clCreateBuffer(context_, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                  input.size_bytes(), const_cast<float *>(input.data()), &status);
        if (status != CL_SUCCESS || !mem)
            return nullptr;
        buffers_.push_back(mem);
        live_ += input.size_bytes();
        peak_ = std::max(peak_, live_);
        return mem;
    }
    cl_mem output_buffer(std::size_t count) {
        cl_int status;
        auto mem =
            clCreateBuffer(context_, CL_MEM_WRITE_ONLY, count * sizeof(float), nullptr, &status);
        if (status != CL_SUCCESS || !mem)
            return nullptr;
        buffers_.push_back(mem);
        live_ += count * sizeof(float);
        peak_ = std::max(peak_, live_);
        return mem;
    }
    Result<std::vector<float>> execute(cl_kernel kernel, std::size_t size, cl_mem output) {
        transfer_.assign(size, 0.0F);
        quiescent_ = false;
        auto status =
            clEnqueueNDRangeKernel(queue_, kernel, 1, nullptr, &size, nullptr, 0, nullptr, nullptr);
        if (status == CL_SUCCESS)
            status =
                clEnqueueReadBuffer(queue_, output, CL_TRUE, 0, transfer_.size() * sizeof(float),
                                    transfer_.data(), 0, nullptr, nullptr);
        auto stopped = synchronize();
        if (status != CL_SUCCESS || !stopped)
            return unavailable<std::vector<float>>();
        if (!std::all_of(transfer_.begin(), transfer_.end(),
                         [](float value) { return std::isfinite(value); }))
            return invalid();
        ++completed_;
        return Result<std::vector<float>>::success(std::move(transfer_));
    }
    bool clear_buffers() noexcept {
        if (!quiescent_)
            return false;
        bool complete = true;
        for (auto &mem : buffers_) {
            if (mem && clReleaseMemObject(mem) == CL_SUCCESS)
                mem = nullptr;
            else if (mem)
                complete = false;
        }
        if (complete) {
            buffers_.clear();
            live_ = 0;
        }
        return complete;
    }
    ComputeDevice device_;
    cl_context context_{nullptr};
    cl_command_queue queue_{nullptr};
    cl_program program_{nullptr};
    cl_kernel matvec_{nullptr}, attention_{nullptr};
    std::vector<cl_mem> buffers_;
    // Retained across uncertain queue failures: an in-flight read may still own this host buffer.
    std::vector<float> transfer_;
    std::size_t bound_{0}, live_{0}, peak_{0};
    std::uint64_t completed_{0};
    bool quiescent_{true};
};
#endif
} // namespace
Result<std::vector<ComputeDevice>> enumerate_opencl_devices() {
#ifdef FLAMORIS_ENABLE_OPENCL
    auto devices = discover();
    if (!devices)
        return Result<std::vector<ComputeDevice>>::failure(devices.error());
    std::vector<ComputeDevice> result;
    for (const auto &entry : devices.value())
        result.push_back(entry.info);
    return Result<std::vector<ComputeDevice>>::success(std::move(result));
#else
    return unavailable<std::vector<ComputeDevice>>();
#endif
}
Result<std::unique_ptr<ComputeImplementation>> make_opencl_compute(std::size_t index,
                                                                   std::size_t bound) {
#ifdef FLAMORIS_ENABLE_OPENCL
    if (!bound || bound > 16777216)
        return unavailable<std::unique_ptr<ComputeImplementation>>();
    auto devices = discover();
    if (!devices || index >= devices.value().size())
        return unavailable<std::unique_ptr<ComputeImplementation>>();
    return OpenClCompute::create(devices.value()[index], bound);
#else
    (void)index;
    (void)bound;
    return unavailable<std::unique_ptr<ComputeImplementation>>();
#endif
}
} // namespace flamoris::runtime
