#include "flamoris/runtime/observation.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace flamoris::runtime {
namespace {
ErrorEnvelope invalid(ErrorReason reason = ErrorReason::limit_exceeded) noexcept {
  return ErrorEnvelope::make(ErrorCode::invalid_request, ErrorStage::validation,
      ExternalOutcome::not_applicable, RetryDisposition::prohibited, reason);
}
ErrorEnvelope capacity() noexcept {
  return ErrorEnvelope::make(ErrorCode::resource_unavailable, ErrorStage::admission,
      ExternalOutcome::not_applicable, RetryDisposition::prohibited, ErrorReason::limit_exceeded);
}
ErrorEnvelope internal() noexcept { return ErrorEnvelope::make(ErrorCode::internal_error); }
std::uint64_t last_sequence(const EventGroup& group) noexcept {
  return group.events.empty() ? group.first_sequence : group.events.back().sequence;
}
bool valid_group(const EventGroup& group, std::uint64_t previous) noexcept {
  if (group.events.empty() || group.transition == 0 || previous == UINT64_MAX ||
      group.first_sequence != previous + 1 || group.events.size() > UINT64_MAX - previous)
    return false;
  auto sequence = group.first_sequence;
  const auto run = group.events.front().job.run();
  for (const auto& event : group.events) {
    if (event.sequence != sequence || !event.job.valid() || event.job.run() != run ||
        !known_event_kind(event.kind) || event.kind.size() > 128) return false;
    if (event.sequence != UINT64_MAX) ++sequence;
  }
  return true;
}
bool is_closure(const EventGroup& group) noexcept {
  return group.events.size() == 1 && group.events.front().kind == "reconciliation.closed";
}
}

bool known_event_kind(const std::string& kind) noexcept {
  constexpr std::array kinds{
    "run.state_changed", "job.state_changed", "continuation.created",
    "continuation.consumed", "continuation.discarded", "interrupt.requested",
    "interrupt.applied", "interrupt.rejected", "attempt.dispatch_committed",
    "attempt.outcome", "race.winner_selected", "join.completed",
    "resource.lease_acquired", "resource.lease_released", "resource.lease_revoked",
    "resource.allocation_release_confirmed", "resource.allocation_unknown",
    "authorization.denied", "budget.reserved", "budget.settled",
    "reconciliation.observed", "reconciliation.closed", "inference.progress",
    "token.generated", "sampling.observed", "telemetry.dropped", "cleanup.pending",
    "run.created", "job.created", "resource.granted", "resource.execution_quiesced",
    "continuation.condition_satisfied", "run.pause_barrier_created", "execution.stopped",
    "resource.release_confirmed", "cleanup.transferred", "run.terminal", "attempt.retry_admitted",
    "group.completed", "inference.stage_changed"
  };
  return std::find(kinds.begin(), kinds.end(), kind) != kinds.end();
}

std::size_t event_group_storage_bytes(const EventGroup& group) noexcept {
  // Runtime-owned payload has no arbitrary strings except a <=128-byte kind.
  // Charge 8 KiB per record, including group/index/projection overhead, as the
  // reviewed reservation profile requires. Reject arithmetic overflow.
  if (group.events.size() > std::numeric_limits<std::size_t>::max() / 8192)
    return std::numeric_limits<std::size_t>::max();
  return group.events.size() * 8192;
}
static_assert(sizeof(LifecycleEvent) + sizeof(EventGroup) + 128 < 8192);
static_assert(std::is_nothrow_move_constructible_v<EventGroup>);

EventStore::EventStore(ObservationLimits limits) : limits_(limits) {
  if (limits_.max_groups > 1048576 || limits_.max_subscribers > 65536 ||
      limits_.max_reconciliations > 65536 || limits_.max_bytes > 1024ULL * 1024 * 1024 ||
      limits_.control_events > 131072 || limits_.emergency_events > 131072 ||
      limits_.reconciliation_events > 131072 || limits_.telemetry_events > 131072 ||
      limits_.max_groups > limits_.max_bytes / (sizeof(EventGroup) + sizeof(StoredGroup)))
    throw std::invalid_argument("invalid observation limits");
  groups_.reserve(limits_.max_groups);
  group_metadata_.reserve(limits_.max_groups);
  subscribers_.reserve(limits_.max_subscribers);
  reconciliations_.reserve(limits_.max_reconciliations);
}

Result<PreparedEventGroup> EventStore::prepare(EventGroup group,
    EventStorageClass storage, std::uint64_t reservation) {
  if (!healthy_ || state_ != ObservationStreamState::open)
    return Result<PreparedEventGroup>::failure(invalid(ErrorReason::stale_observation));
  if (!valid_group(group, watermark_))
    return Result<PreparedEventGroup>::failure(invalid(ErrorReason::invalid_transition));
  if (group.transition <= last_transition_ ||
      (run_ && group.events.front().job.run() != *run_))
    return Result<PreparedEventGroup>::failure(invalid(ErrorReason::stale_observation));
  const auto bytes = event_group_storage_bytes(group);
  const auto count = group.events.size();
  bool fits = groups_.size() < limits_.max_groups && bytes <= limits_.max_bytes &&
      bytes_ <= limits_.max_bytes - bytes;
  // Ordinary and telemetry records cannot consume a prepaid cleanup closure.
  if (storage != EventStorageClass::reconciliation) {
    const auto protected_slots = reserved_reconciliation_ +
        (storage == EventStorageClass::emergency ? 0 : limits_.emergency_events - emergency_);
    fits = fits && protected_slots < limits_.max_groups - groups_.size() &&
        protected_slots <= limits_.max_bytes / 8192 &&
        bytes <= limits_.max_bytes - protected_slots * 8192 &&
        bytes_ <= limits_.max_bytes - protected_slots * 8192 - bytes;
  }
  switch (storage) {
    case EventStorageClass::control:
      fits = fits && count <= limits_.control_events && control_ <= limits_.control_events - count;
      break;
    case EventStorageClass::emergency:
      fits = fits && count <= limits_.emergency_events && emergency_ <= limits_.emergency_events - count;
      break;
    case EventStorageClass::telemetry:
      fits = fits && count <= limits_.telemetry_events && telemetry_ <= limits_.telemetry_events - count;
      if (!fits) telemetry_dropped_ = count > UINT64_MAX - telemetry_dropped_ ?
          UINT64_MAX : telemetry_dropped_ + count;
      break;
    case EventStorageClass::reconciliation: {
      auto it = std::find_if(reconciliations_.begin(), reconciliations_.end(),
          [reservation](const auto& r) { return r.id == reservation; });
      fits = fits && count == 1 && it != reconciliations_.end() && !it->closed &&
          (is_closure(group) || (it->updates_remaining != 0 &&
           group.events.front().kind == "reconciliation.observed"));
      break;
    }
  }
  if (!fits) return Result<PreparedEventGroup>::failure(capacity());
  return Result<PreparedEventGroup>::success(PreparedEventGroup(
      std::move(group), storage, bytes, watermark_, reservation, this));
}

bool EventStore::publish(PreparedEventGroup&& prepared) noexcept {
  if (!healthy_ || state_ != ObservationStreamState::open || prepared.owner_ != this ||
      watermark_ != prepared.expected_watermark_ || groups_.size() == groups_.capacity() ||
      group_metadata_.size() == group_metadata_.capacity()) {
    healthy_ = false;
    return false;
  }
  const auto count = prepared.group_.events.size();
  switch (prepared.storage_) {
    case EventStorageClass::control: control_ += count; break;
    case EventStorageClass::emergency: emergency_ += count; break;
    case EventStorageClass::telemetry: telemetry_ += count; break;
    case EventStorageClass::reconciliation: {
      auto it = std::find_if(reconciliations_.begin(), reconciliations_.end(),
          [&](const auto& r) { return r.id == prepared.reservation_; });
      if (it == reconciliations_.end() || it->closed) { healthy_ = false; return false; }
      if (is_closure(prepared.group_)) {
        reserved_reconciliation_ -= it->updates_remaining + 1;
        it->updates_remaining = 0;
        it->closed = true;
      } else { --it->updates_remaining; --reserved_reconciliation_; }
      reconciliation_ += count;
      break;
    }
  }
  watermark_ = last_sequence(prepared.group_);
  last_transition_ = prepared.group_.transition;
  run_ = prepared.group_.events.front().job.run();
  bytes_ += prepared.bytes_;
  group_metadata_.push_back({prepared.storage_, prepared.bytes_});
  groups_.push_back(std::move(prepared.group_));
  return true;
}

std::uint64_t EventStore::earliest_sequence() const noexcept {
  return groups_.empty() ? (watermark_ == UINT64_MAX ? UINT64_MAX : watermark_ + 1)
                         : groups_.front().first_sequence;
}
std::size_t EventStore::retained_events() const noexcept {
  return control_ + emergency_ + reconciliation_ + telemetry_;
}
Result<ObservationPage> EventStore::read(std::uint64_t after, std::size_t max_events,
    std::size_t max_bytes) const {
  if (!max_events || max_events > 65536 || !max_bytes || max_bytes > 64 * 1024 * 1024 || after > watermark_)
    return Result<ObservationPage>::failure(invalid());
  try {
    ObservationPage page;
    page.earliest_retained_sequence = earliest_sequence();
    page.watermark = watermark_;
    page.next_after = after;
    page.telemetry_dropped = telemetry_dropped_;
    page.stream_state = state_;
    if (state_ == ObservationStreamState::expired || (after < watermark_ &&
        after + 1 < page.earliest_retained_sequence))
      page.gap = ObservationGap{after, page.earliest_retained_sequence, watermark_, false,
                                state_ == ObservationStreamState::expired};
    std::size_t count = 0, bytes = 0;
    for (std::size_t i = 0; i < groups_.size(); ++i) {
      const auto& group = groups_[i];
      if (last_sequence(group) <= after) continue;
      if (group.first_sequence <= after) {
        page.incomplete_group = IncompleteGroup{group.transition, group.first_sequence, group.events.size(), 0};
        page.gap = ObservationGap{after, group.first_sequence, watermark_, false, false};
        page.next_after = last_sequence(group);
        continue;
      }
      if (group.events.size() > max_events - count || group_metadata_[i].bytes > max_bytes - bytes) {
        if (page.groups.empty()) page.incomplete_group = IncompleteGroup{
            group.transition, group.first_sequence, group.events.size(), 0};
        break;
      }
      page.groups.push_back(group);
      count += group.events.size(); bytes += group_metadata_[i].bytes;
      page.next_after = last_sequence(group);
    }
    return Result<ObservationPage>::success(std::move(page));
  } catch (...) { return Result<ObservationPage>::failure(internal()); }
}

Result<std::uint64_t> EventStore::subscribe(std::uint64_t after, std::size_t capacity_events) {
  if (!capacity_events || capacity_events > 65536 || after > watermark_ || state_ == ObservationStreamState::expired)
    return Result<std::uint64_t>::failure(invalid());
  if (next_subscriber_ == UINT64_MAX) return Result<std::uint64_t>::failure(capacity());
  auto vacant = std::find_if(subscribers_.begin(), subscribers_.end(), [](const auto& s) { return !s.active; });
  if (vacant == subscribers_.end() && subscribers_.size() == limits_.max_subscribers)
    return Result<std::uint64_t>::failure(capacity());
  const auto id = next_subscriber_++;
  if (vacant == subscribers_.end()) subscribers_.push_back({id, after, capacity_events, true});
  else *vacant = Subscriber{id, after, capacity_events, true};
  return Result<std::uint64_t>::success(id);
}
Result<ObservationPage> EventStore::poll(std::uint64_t id, std::size_t max_events) {
  auto it = std::find_if(subscribers_.begin(), subscribers_.end(),
      [id](const auto& s) { return s.id == id && s.active; });
  if (it == subscribers_.end()) return Result<ObservationPage>::failure(invalid(ErrorReason::stale_observation));
  const auto requested = it->cursor;
  bool overflow = watermark_ - it->cursor > it->capacity;
  if (overflow) {
    const auto floor = watermark_ - it->capacity;
    it->cursor = floor;
    for (const auto& group : groups_)
      if (group.first_sequence <= floor && last_sequence(group) > floor) it->cursor = last_sequence(group);
  }
  auto result = read(it->cursor, max_events);
  if (!result) return result;
  if (overflow) result.value().gap = ObservationGap{requested,
      it->cursor == UINT64_MAX ? UINT64_MAX : it->cursor + 1, watermark_, true,
      state_ == ObservationStreamState::expired};
  it->cursor = result.value().next_after;
  return result;
}
void EventStore::unsubscribe(std::uint64_t id) noexcept {
  for (auto& subscriber : subscribers_) if (subscriber.id == id) subscriber.active = false;
}
std::size_t EventStore::subscriber_count() const noexcept {
  return static_cast<std::size_t>(std::count_if(subscribers_.begin(), subscribers_.end(),
      [](const auto& s) { return s.active; }));
}
void EventStore::retire_through(std::uint64_t sequence) noexcept {
  std::size_t count = 0;
  while (count < groups_.size() && last_sequence(groups_[count]) <= sequence) {
    const auto events = groups_[count].events.size();
    bytes_ -= group_metadata_[count].bytes;
    switch (group_metadata_[count].storage) {
      case EventStorageClass::control: control_ -= events; break;
      case EventStorageClass::emergency: emergency_ -= events; break;
      case EventStorageClass::reconciliation: reconciliation_ -= events; break;
      case EventStorageClass::telemetry: telemetry_ -= events; break;
    }
    ++count;
  }
  groups_.erase(groups_.begin(), groups_.begin() + static_cast<std::ptrdiff_t>(count));
  group_metadata_.erase(group_metadata_.begin(), group_metadata_.begin() + static_cast<std::ptrdiff_t>(count));
}
bool EventStore::close() noexcept {
  if (reserved_reconciliation_ != 0) return false;
  if (state_ == ObservationStreamState::open) state_ = ObservationStreamState::closed;
  return true;
}
bool EventStore::expire() noexcept {
  if (!close()) return false;
  state_ = ObservationStreamState::expired; retire_through(watermark_); return true;
}
Result<std::uint64_t> EventStore::reserve_reconciliation(std::size_t max_updates) {
  if (!healthy_ || state_ != ObservationStreamState::open || max_updates > 4 ||
      reconciliations_.size() == limits_.max_reconciliations || next_reconciliation_ == UINT64_MAX)
    return Result<std::uint64_t>::failure(capacity());
  const auto count = max_updates + 1;
  if (count > limits_.reconciliation_events ||
      reconciliation_ > limits_.reconciliation_events - count ||
      reserved_reconciliation_ > limits_.reconciliation_events - count - reconciliation_ ||
      reserved_reconciliation_ > limits_.max_groups - groups_.size() ||
      count > limits_.max_groups - groups_.size() - reserved_reconciliation_ ||
      count + reserved_reconciliation_ > limits_.max_bytes / 8192 ||
      bytes_ > limits_.max_bytes - (count + reserved_reconciliation_) * 8192)
    return Result<std::uint64_t>::failure(capacity());
  const auto id = next_reconciliation_++;
  reconciliations_.push_back({id, max_updates, false});
  reserved_reconciliation_ += count;
  return Result<std::uint64_t>::success(id);
}
bool EventStore::reconciliation_reserved(std::uint64_t reservation) const noexcept {
  return std::any_of(reconciliations_.begin(), reconciliations_.end(),
      [reservation](const auto& r) { return r.id == reservation && !r.closed; });
}

} // namespace flamoris::runtime
