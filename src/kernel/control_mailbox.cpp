#include "flamoris/runtime/control_mailbox.hpp"
#include <condition_variable>
#include <limits>
#include <mutex>
#include <vector>

namespace flamoris::runtime {
namespace detail {
struct ControlMailboxState {
    struct Slot {
        std::uint64_t generation{};
        std::optional<CallbackCorrelation> correlation;
        std::optional<ControlMessage> message;
    };
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::optional<ControlMessage>> normal;
    std::vector<Slot> completions;
    std::size_t read{}, write{}, count{}, next_completion{};
    bool normal_closed{}, reservations_closed{}, closed{};
    std::optional<ControlMessage> take_locked() noexcept {
        for (std::size_t offset = 0; offset < completions.size(); ++offset) {
            const auto i = (next_completion + offset) % completions.size();
            auto& slot = completions[i];
            if (slot.message) {
                auto message = std::move(slot.message);
                slot.message.reset();
                slot.correlation.reset();
                next_completion = (i + 1) % completions.size();
                return message;
            }
        }
        if (!count) return std::nullopt;
        auto message = std::move(normal[read]);
        normal[read].reset();
        read = (read + 1) % normal.size();
        --count;
        return message;
    }
    bool ready_locked() const noexcept {
        if (closed || count) return true;
        for (const auto& slot : completions) if (slot.message) return true;
        return false;
    }
};
}
namespace {
ErrorEnvelope unavailable(ErrorReason reason = ErrorReason::limit_exceeded) noexcept {
    return ErrorEnvelope::make(ErrorCode::resource_unavailable, ErrorStage::admission,
        ExternalOutcome::not_applicable, RetryDisposition::prohibited, reason);
}
bool valid(const CallbackCorrelation& correlation) noexcept {
    return std::visit([](const auto& c) {
        using T = std::decay_t<decltype(c)>;
        if constexpr (std::is_same_v<T, AdmissionCorrelation>)
            return c.instance.valid() && c.submission.valid() && c.generation.valid();
        else if constexpr (std::is_same_v<T, LifecycleCorrelation>)
            return c.job.valid() && c.attempt.valid() && c.operation.valid() && c.dispatch.valid();
        else
            return c.instance.valid() && c.cleanup.valid() && c.allocation.valid() &&
                c.operation.valid() && c.host.valid() && c.worker.valid();
    }, correlation);
}
}
Result<std::unique_ptr<ControlMailbox>> ControlMailbox::create(std::size_t normal, std::size_t completions) {
    if (!normal || !completions || normal > 1024 || completions > 65536)
        return Result<std::unique_ptr<ControlMailbox>>::failure(unavailable());
    try {
        auto state = std::make_shared<detail::ControlMailboxState>();
        state->normal.resize(normal);
        state->completions.resize(completions);
        return Result<std::unique_ptr<ControlMailbox>>::success(
            std::unique_ptr<ControlMailbox>(new ControlMailbox(std::move(state))));
    } catch (...) {
        return Result<std::unique_ptr<ControlMailbox>>::failure(unavailable());
    }
}
ControlMailbox::~ControlMailbox() {
    // Endpoint shutdown only. The Runtime must prove native quiescence before
    // destroying its worker holders; this class owns no native allocations.
    std::lock_guard lock(state_->mutex);
    state_->closed = true;
    state_->changed.notify_all();
}
DeliveryStatus CompletionEndpoint::publish(ControlMessage message) const noexcept {
    const auto state = state_.lock();
    if (!state) return DeliveryStatus::closed;
    std::lock_guard lock(state->mutex);
    if (state->closed) return DeliveryStatus::closed;
    if (slot_.index >= state->completions.size()) return DeliveryStatus::stale;
    auto& slot = state->completions[slot_.index];
    if (slot.generation != slot_.generation || !slot.correlation || *slot.correlation != message.correlation)
        return DeliveryStatus::stale;
    if (slot.message) return *slot.message == message ? DeliveryStatus::duplicate : DeliveryStatus::stale;
    slot.message = std::move(message);
    state->changed.notify_one();
    return DeliveryStatus::delivered;
}
Result<void> ControlMailbox::post(ControlMessage message) {
    if (!valid(message.correlation)) return Result<void>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->normal_closed || state_->count == state_->normal.size())
        return Result<void>::failure(unavailable());
    state_->normal[state_->write] = std::move(message);
    state_->write = (state_->write + 1) % state_->normal.size();
    ++state_->count;
    state_->changed.notify_one();
    return Result<void>::success();
}
Result<CompletionEndpoint> ControlMailbox::reserve(CallbackCorrelation correlation) {
    if (!valid(correlation)) return Result<CompletionEndpoint>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->reservations_closed) return Result<CompletionEndpoint>::failure(unavailable());
    for (const auto& slot : state_->completions)
        if (slot.correlation && *slot.correlation == correlation)
            return Result<CompletionEndpoint>::failure(unavailable(ErrorReason::stale_observation));
    for (std::size_t i=0; i<state_->completions.size(); ++i) {
        auto& slot=state_->completions[i];
        if (!slot.correlation && slot.generation != std::numeric_limits<std::uint64_t>::max()) {
            ++slot.generation;
            slot.correlation=std::move(correlation);
            return Result<CompletionEndpoint>::success(CompletionEndpoint(state_, {i,slot.generation}));
        }
    }
    return Result<CompletionEndpoint>::failure(unavailable());
}
std::optional<ControlMessage> ControlMailbox::take() noexcept {
    std::lock_guard lock(state_->mutex);
    return state_->take_locked();
}
std::optional<ControlMessage> ControlMailbox::wait_take() noexcept {
    std::unique_lock lock(state_->mutex);
    state_->changed.wait(lock,[&] { return state_->ready_locked(); });
    return state_->take_locked();
}
void ControlMailbox::close_normal_ingress() noexcept {
    std::lock_guard lock(state_->mutex);
    state_->normal_closed=true;
}
void ControlMailbox::close_reservations() noexcept {
    std::lock_guard lock(state_->mutex);
    state_->reservations_closed=true;
}
Result<void> ControlMailbox::withdraw_unstarted(CompletionSlotId id) {
    std::lock_guard lock(state_->mutex);
    if (id.index>=state_->completions.size()) return Result<void>::failure(unavailable(ErrorReason::stale_observation));
    auto& slot=state_->completions[id.index];
    if (slot.generation!=id.generation || !slot.correlation || slot.message)
        return Result<void>::failure(unavailable(ErrorReason::stale_observation));
    slot.correlation.reset();
    return Result<void>::success();
}
Result<void> ControlMailbox::close_after_drain() {
    std::lock_guard lock(state_->mutex);
    if (!state_->normal_closed || !state_->reservations_closed || state_->count)
        return Result<void>::failure(unavailable());
    for (const auto& slot:state_->completions) if(slot.correlation) return Result<void>::failure(unavailable());
    state_->closed=true;
    state_->changed.notify_all();
    return Result<void>::success();
}
MailboxSnapshot ControlMailbox::snapshot() const noexcept {
    std::lock_guard lock(state_->mutex);
    MailboxSnapshot snapshot{state_->count,0,0,state_->normal_closed,state_->reservations_closed,state_->closed};
    for (const auto& slot:state_->completions) {
        snapshot.reserved_completions+=slot.correlation.has_value();
        snapshot.ready_completions+=slot.message.has_value();
    }
    return snapshot;
}
} // namespace flamoris::runtime
