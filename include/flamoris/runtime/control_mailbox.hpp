#pragma once

#include "flamoris/runtime/ids.hpp"
#include "flamoris/runtime/result.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

namespace flamoris::runtime {

struct AdmissionCorrelation {
    RuntimeInstanceId instance;
    PendingSubmissionId submission;
    OperationGeneration generation;
    bool operator==(const AdmissionCorrelation&) const = default;
};
struct LifecycleCorrelation {
    JobId job;
    AttemptId attempt;
    OperationId operation;
    DispatchGeneration dispatch;
    SuspensionGeneration suspension;
    bool operator==(const LifecycleCorrelation&) const = default;
};
struct CleanupCorrelation {
    RuntimeInstanceId instance;
    CleanupId cleanup;
    AllocationId allocation;
    OperationId operation;
    HostEpoch host;
    NativeWorkerGeneration worker;
    bool operator==(const CleanupCorrelation&) const = default;
};
using CallbackCorrelation = std::variant<AdmissionCorrelation, LifecycleCorrelation, CleanupCorrelation>;
enum class ControlMessageKind { command, validation_ready, native_ready, adapter_ready, resource_ready, cleanup_ready };

// Larger results live in separately charged storage. A callback carries only its
// immutable identity and the opaque owner-managed handle, never a controller pointer.
struct ControlMessage {
    ControlMessageKind kind{ControlMessageKind::command};
    CallbackCorrelation correlation;
    OperationId payload_handle;
    std::optional<ErrorCode> error;
    bool operator==(const ControlMessage&) const = default;
};
struct CompletionSlotId {
    std::size_t index{};
    std::uint64_t generation{};
    bool operator==(const CompletionSlotId&) const = default;
};
enum class DeliveryStatus { delivered, duplicate, stale, closed };
namespace detail { struct ControlMailboxState; }

class CompletionEndpoint final {
public:
    CompletionEndpoint() = default;
    [[nodiscard]] DeliveryStatus publish(ControlMessage) const noexcept;
    [[nodiscard]] CompletionSlotId slot() const noexcept { return slot_; }
private:
    friend class ControlMailbox;
    CompletionEndpoint(std::weak_ptr<detail::ControlMailboxState> state, CompletionSlotId slot)
        : state_(std::move(state)), slot_(slot) {}
    std::weak_ptr<detail::ControlMailboxState> state_;
    CompletionSlotId slot_;
};
struct MailboxSnapshot {
    std::size_t normal_pending{}, reserved_completions{}, ready_completions{};
    bool normal_closed{}, reservations_closed{}, closed{};
};

// A bounded thread-safe transport, not execution authority. Exactly one control
// consumer drains it. Delivery never calls user code or mutates lifecycle state.
class ControlMailbox final {
public:
    static Result<std::unique_ptr<ControlMailbox>> create(std::size_t normal_capacity,
                                                          std::size_t completion_capacity);
    ~ControlMailbox();
    ControlMailbox(const ControlMailbox&) = delete;
    ControlMailbox& operator=(const ControlMailbox&) = delete;
    Result<void> post(ControlMessage);
    Result<CompletionEndpoint> reserve(CallbackCorrelation);
    [[nodiscard]] std::optional<ControlMessage> take() noexcept;
    // Wait uses a condition variable; closure wakes it. Process after this call,
    // outside the inbox mutex. Deterministic tests use take and barriers instead.
    [[nodiscard]] std::optional<ControlMessage> wait_take() noexcept;
    void close_normal_ingress() noexcept;
    void close_reservations() noexcept;
    // Called only for preparation proved never handed to a worker. An endpoint
    // destructor, elapsed time or stale generation is not such proof.
    Result<void> withdraw_unstarted(CompletionSlotId);
    Result<void> close_after_drain();
    [[nodiscard]] MailboxSnapshot snapshot() const noexcept;
private:
    explicit ControlMailbox(std::shared_ptr<detail::ControlMailboxState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::ControlMailboxState> state_;
};
static_assert(sizeof(ControlMessage) <= 2048);
} // namespace flamoris::runtime
