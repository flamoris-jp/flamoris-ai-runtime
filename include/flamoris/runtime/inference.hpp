#pragma once
#include "flamoris/runtime/lifecycle.hpp"
#include "flamoris/runtime/native.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace flamoris::runtime {
enum class NativeOperation { load, step, pause, resume, inject, stop, graceful_stop, release };
struct NativeWorkerConfig {
    // Trusted registry artifacts only. A Workflow cannot provide this path/checksum.
    std::filesystem::path registered_artifact;
    std::string artifact_sha256;
    std::shared_ptr<const TinyModel> shared_model;
    ProcessorDefinition processor;
    TokenizerDefinition tokenizer;
    std::string prompt;
    NativeOptions options;
    bool opencl{false};
    std::string compute_identity{"flamoris.cpu-fp32.v1"};
    // Registered conservative driver/context allowance, retained across suspension.
    std::size_t retained_context_bytes{0};
    std::size_t device_index{0};
    std::size_t device_memory_bound{65536};
};
struct NativeCommand {
    DispatchTicket ticket;
    std::uint64_t operation_generation{0};
    NativeOperation operation{NativeOperation::step};
    std::size_t prefill_tokens{8};
    NativePins resume_pins;
    std::uint64_t suspension_generation{0}, injection_command{0};
    std::string input;
};
struct NativeObservation {
    DispatchTicket ticket;
    std::uint64_t operation_generation{0};
    NativeOperation operation{NativeOperation::step};
    NativeExecutionState state;
    SegmentReceipt segment;
    NativeReleaseReceipt release;
    // Successful load only: Runtime must transfer this immutable holder into its
    // explicitly accounted residency record, then clear the receipt's reference.
    std::shared_ptr<const TinyModel> resident_model;
    std::uint64_t suspension_generation{0};
    std::size_t model_bytes{0}, state_bytes{0};
    bool quiescent{false}, model_allocation_released{false}, control_rejected{false};
    std::optional<ErrorEnvelope> error;
};
// The worker owns all native state. Its bounded request/completion slots contain
// values only; neither this port nor a native thread can dereference a controller.
class NativeWorkerPort {
  public:
    virtual ~NativeWorkerPort() = default;
    virtual Result<void> submit(NativeCommand) = 0;
    virtual std::optional<NativeObservation> take() = 0;
    virtual bool idle() const noexcept = 0;
    virtual Result<void> close() = 0;
};
class ThreadNativeWorker final : public NativeWorkerPort {
  public:
    // Trusted composition seam; the default creates the configured native compute.
    using ComputeFactory = std::function<ComputePreparation(const NativeWorkerConfig &)>;
    static Result<std::unique_ptr<ThreadNativeWorker>> create(NativeWorkerConfig,
                                                              ComputeFactory = {});
    ~ThreadNativeWorker() override;
    Result<void> submit(NativeCommand) override;
    std::optional<NativeObservation> take() override;
    bool idle() const noexcept override;
    // Only after actual release receipt has been consumed, outside control thread.
    Result<void> close() override;
    std::optional<NativeObservation> wait_take(std::chrono::milliseconds timeout);

  private:
    struct Impl;
    explicit ThreadNativeWorker(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};
// Control-thread owner; submits bounded commands and accepts fenced observations.
// Resource reservation/release reconciliation stays with Runtime/ResourceManager.
class InferenceMachine {
  public:
    InferenceMachine(std::unique_ptr<NativeWorkerPort>, std::uint64_t state_reference);
    Result<void> submit(RunController &, DispatchTicket, NativeOperation, DispatchChecks,
                        NativeCommand payload = {});
    std::optional<NativeObservation> take();
    Result<void> accept(RunController &, const NativeObservation &);
    Result<SpawnResult> accept_with_children(RunController &, const NativeObservation &,
                                             std::span<const ChildSpec>);
    Result<void> close();
    bool pending() const noexcept { return pending_.has_value(); }
    const std::optional<NativeExecutionState> &native_state() const noexcept { return state_; }

  private:
    std::unique_ptr<NativeWorkerPort> worker_;
    std::uint64_t state_reference_{0}, generation_{0};
    std::optional<NativeCommand> pending_;
    std::optional<NativeExecutionState> state_;
};
} // namespace flamoris::runtime
