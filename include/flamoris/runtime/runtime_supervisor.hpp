#pragma once

#include "flamoris/runtime/resources.hpp"
#include <array>
#include <memory>
#include <mutex>

namespace flamoris::runtime {

// Cleanup receipts remain deliverable after the public Runtime wrapper is gone.
// Implementations enqueue owned values on the retained control executor and still
// validate the complete resource/operation identity. This interface admits no work.
class RuntimeCleanupEndpoint {
  public:
    virtual ~RuntimeCleanupEndpoint() = default;
    virtual Result<void> observe_host_envelope(HostEnvelope) = 0;
    virtual Result<void> observe_host_acquisition(HostAcquisition) = 0;
    virtual Result<void> observe_host_release(HostReleaseReceipt) = 0;
};

class RuntimeRetainedOwner {
  public:
    virtual ~RuntimeRetainedOwner() = default;
    // An atomic cached proof: no provider/native call or callback can still
    // access work memory, every owned resource is settled, and no useful work
    // can start. The control actor may remain alive solely for cleanup.
    virtual bool cleanup_complete() const noexcept = 0;
    // Called once, outside the control actor and registry mutex, only after the
    // above proof. Stop/join the actor and already-quiescent workers here.
    virtual void close_and_join() noexcept = 0;
};

class RuntimeSupervisor;
class RuntimeRetentionSlot final {
  public:
    RuntimeRetentionSlot() noexcept = default;
    ~RuntimeRetentionSlot();
    RuntimeRetentionSlot(RuntimeRetentionSlot &&) noexcept;
    RuntimeRetentionSlot &operator=(RuntimeRetentionSlot &&) noexcept;
    RuntimeRetentionSlot(const RuntimeRetentionSlot &) = delete;
    RuntimeRetentionSlot &operator=(const RuntimeRetentionSlot &) = delete;
    bool valid() const noexcept { return supervisor_ != nullptr; }
    // No allocation and no remote operation. The valid pre-reserved slot takes
    // the entire owner; the caller must already have closed useful admission.
    bool retain(std::shared_ptr<RuntimeRetainedOwner>) noexcept;

  private:
    friend class RuntimeSupervisor;
    RuntimeRetentionSlot(RuntimeSupervisor *, std::size_t, std::uint64_t) noexcept;
    void reset() noexcept;
    RuntimeSupervisor *supervisor_{};
    std::size_t index_{};
    std::uint64_t generation_{};
};

struct RuntimeSupervisorSnapshot {
    std::size_t capacity{}, reserved{}, retained{}, reclaiming{};
};

// This bounded process-lifetime owner is deliberately never destroyed during
// static teardown: unresolved writers cannot be freed or joined by a destructor.
// It creates no execution thread, retries no operation, and holds no Run authority.
class RuntimeSupervisor final {
  public:
    static constexpr std::size_t capacity = 128;
    static RuntimeSupervisor &process();
    Result<RuntimeRetentionSlot> reserve();
    // Invoke from an external management thread, never a retained control actor.
    // Reclamation only joins workers for which cleanup_complete already proved
    // quiescence. Unknown owners keep their slots without a timeout refund.
    std::size_t collect() noexcept;
    RuntimeSupervisorSnapshot snapshot() const noexcept;
    RuntimeSupervisor(const RuntimeSupervisor &) = delete;
    RuntimeSupervisor &operator=(const RuntimeSupervisor &) = delete;

  private:
    friend class RuntimeRetentionSlot;
    RuntimeSupervisor() = default;
    enum class State { free, reserved, retained, reclaiming, exhausted };
    struct Slot {
        State state{State::free};
        std::uint64_t generation{};
        std::shared_ptr<RuntimeRetainedOwner> owner;
    };
    bool retain(std::size_t, std::uint64_t, std::shared_ptr<RuntimeRetainedOwner>) noexcept;
    void release_reservation(std::size_t, std::uint64_t) noexcept;
    mutable std::mutex mutex_;
    std::array<Slot, capacity> slots_{};
};

} // namespace flamoris::runtime
