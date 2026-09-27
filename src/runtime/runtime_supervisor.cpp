#include "flamoris/runtime/runtime_supervisor.hpp"
#include <limits>
#include <utility>

namespace flamoris::runtime {

RuntimeRetentionSlot::RuntimeRetentionSlot(RuntimeSupervisor *supervisor, std::size_t index,
                                           std::uint64_t generation) noexcept
    : supervisor_(supervisor), index_(index), generation_(generation) {}
RuntimeRetentionSlot::~RuntimeRetentionSlot() { reset(); }
RuntimeRetentionSlot::RuntimeRetentionSlot(RuntimeRetentionSlot &&other) noexcept
    : supervisor_(std::exchange(other.supervisor_, nullptr)), index_(other.index_),
      generation_(other.generation_) {}
RuntimeRetentionSlot &RuntimeRetentionSlot::operator=(RuntimeRetentionSlot &&other) noexcept {
    if (this != &other) {
        reset();
        supervisor_ = std::exchange(other.supervisor_, nullptr);
        index_ = other.index_;
        generation_ = other.generation_;
    }
    return *this;
}
void RuntimeRetentionSlot::reset() noexcept {
    if (supervisor_)
        supervisor_->release_reservation(index_, generation_);
    supervisor_ = nullptr;
}
bool RuntimeRetentionSlot::retain(std::shared_ptr<RuntimeRetainedOwner> owner) noexcept {
    if (!supervisor_ || !owner || !supervisor_->retain(index_, generation_, std::move(owner)))
        return false;
    supervisor_ = nullptr;
    return true;
}

RuntimeSupervisor &RuntimeSupervisor::process() {
    // Explicitly process-lifetime. Static destruction must not free unresolved
    // native/provider ownership, and the finite array bounds that retention.
    static auto *const supervisor = new RuntimeSupervisor;
    return *supervisor;
}
Result<RuntimeRetentionSlot> RuntimeSupervisor::reserve() {
    (void)collect();
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        auto &slot = slots_[i];
        if (slot.state != State::free)
            continue;
        if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
            slot.state = State::exhausted;
            continue;
        }
        ++slot.generation;
        slot.state = State::reserved;
        return Result<RuntimeRetentionSlot>::success(
            RuntimeRetentionSlot(this, i, slot.generation));
    }
    return Result<RuntimeRetentionSlot>::failure(ErrorEnvelope::make(
        ErrorCode::resource_unavailable, ErrorStage::admission, ExternalOutcome::not_applicable,
        RetryDisposition::prohibited, ErrorReason::limit_exceeded));
}
bool RuntimeSupervisor::retain(std::size_t index, std::uint64_t generation,
                               std::shared_ptr<RuntimeRetainedOwner> owner) noexcept {
    std::lock_guard lock(mutex_);
    if (index >= slots_.size() || !owner)
        return false;
    auto &slot = slots_[index];
    if (slot.state != State::reserved || slot.generation != generation)
        return false;
    for (const auto &existing : slots_)
        if (existing.owner == owner)
            return false;
    slot.owner = std::move(owner);
    slot.state = State::retained;
    return true;
}
void RuntimeSupervisor::release_reservation(std::size_t index, std::uint64_t generation) noexcept {
    std::lock_guard lock(mutex_);
    if (index >= slots_.size())
        return;
    auto &slot = slots_[index];
    if (slot.state == State::reserved && slot.generation == generation)
        slot.state = State::free;
}
std::size_t RuntimeSupervisor::collect() noexcept {
    struct Candidate {
        std::shared_ptr<RuntimeRetainedOwner> owner;
        std::uint64_t generation{};
    };
    std::array<Candidate, capacity> candidates{};
    {
        std::lock_guard lock(mutex_);
        for (std::size_t i = 0; i < slots_.size(); ++i)
            if (slots_[i].state == State::retained)
                candidates[i] = {slots_[i].owner, slots_[i].generation};
    }
    std::size_t collected = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        auto &candidate = candidates[i];
        if (!candidate.owner || !candidate.owner->cleanup_complete())
            continue;
        {
            std::lock_guard lock(mutex_);
            auto &slot = slots_[i];
            if (slot.state != State::retained || slot.generation != candidate.generation ||
                slot.owner != candidate.owner)
                continue;
            slot.state = State::reclaiming;
        }
        // The owner contract makes this a quiescent join, never a remote wait.
        candidate.owner->close_and_join();
        {
            std::lock_guard lock(mutex_);
            auto &slot = slots_[i];
            slot.owner.reset();
            slot.state = State::free;
        }
        candidate.owner.reset();
        ++collected;
    }
    return collected;
}
RuntimeSupervisorSnapshot RuntimeSupervisor::snapshot() const noexcept {
    std::lock_guard lock(mutex_);
    RuntimeSupervisorSnapshot result{capacity};
    for (const auto &slot : slots_) {
        result.reserved += slot.state == State::reserved;
        result.retained += slot.state == State::retained;
        result.reclaiming += slot.state == State::reclaiming;
    }
    return result;
}

} // namespace flamoris::runtime
