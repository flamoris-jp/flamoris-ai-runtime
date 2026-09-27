#pragma once
#include "flamoris/runtime/lifecycle_events.hpp"
#include "flamoris/runtime/result.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flamoris::runtime {
struct AuthorizationContext;
struct PolicySnapshot;

enum class EventStorageClass { control, emergency, reconciliation, telemetry };
enum class ObservationStreamState { open, closed, expired };
struct ObservationLimits {
  std::size_t max_groups{8192};
  std::size_t control_events{4096};
  std::size_t emergency_events{128};
  std::size_t reconciliation_events{128};
  std::size_t telemetry_events{256};
  std::size_t max_bytes{64 * 1024 * 1024};
  std::size_t max_subscribers{16};
  std::size_t max_reconciliations{32};
};
struct ObservationGap {
  std::uint64_t requested_after{0}, earliest_available{0}, watermark{0};
  bool subscriber_overflow{false}, expired{false};
};
struct IncompleteGroup {
  std::uint64_t transition{0}, first_sequence{0};
  std::size_t expected_events{0}, available_events{0};
};
struct ObservationPage {
  std::vector<EventGroup> groups;
  std::optional<ObservationGap> gap;
  std::optional<IncompleteGroup> incomplete_group;
  std::uint64_t earliest_retained_sequence{1}, watermark{0}, next_after{0};
  std::uint64_t telemetry_dropped{0};
  ObservationStreamState stream_state{ObservationStreamState::open};
};

// This is a prepaid immutable publication value, not a second commit authority.
// Only the owning controller calls prepare/publish in its serialized control turn.
class PreparedEventGroup {
 public:
  PreparedEventGroup(PreparedEventGroup&&) noexcept = default;
  PreparedEventGroup& operator=(PreparedEventGroup&&) noexcept = default;
  PreparedEventGroup(const PreparedEventGroup&) = delete;
  PreparedEventGroup& operator=(const PreparedEventGroup&) = delete;
 private:
  friend class EventStore;
  PreparedEventGroup(EventGroup group, EventStorageClass storage, std::size_t bytes,
                     std::uint64_t watermark, std::uint64_t reservation, const void* owner)
      : group_(std::move(group)), storage_(storage), bytes_(bytes),
        expected_watermark_(watermark), reservation_(reservation), owner_(owner) {}
  EventGroup group_;
  EventStorageClass storage_;
  std::size_t bytes_;
  std::uint64_t expected_watermark_, reservation_;
  const void* owner_;
};

class EventStore {
 public:
  explicit EventStore(ObservationLimits limits = {});
  EventStore(const EventStore&) = delete;
  EventStore& operator=(const EventStore&) = delete;
  EventStore(EventStore&&) = delete;
  EventStore& operator=(EventStore&&) = delete;
  Result<PreparedEventGroup> prepare(EventGroup group,
      EventStorageClass storage = EventStorageClass::control,
      std::uint64_t reconciliation_reservation = 0);
  // false means an invariant breach: the owner must enter fail-stop containment.
  // No allocation, callback, serialization, or observer acknowledgement occurs here.
  bool publish(PreparedEventGroup&& prepared) noexcept;
  Result<ObservationPage> read(std::uint64_t after_sequence,
      std::size_t max_events = 256, std::size_t max_bytes = 1024 * 1024) const;
  Result<std::uint64_t> subscribe(std::uint64_t after_sequence, std::size_t buffered_events);
  Result<ObservationPage> poll(std::uint64_t subscriber, std::size_t max_events = 256);
  void unsubscribe(std::uint64_t subscriber) noexcept;
  // Active bookkeeping determines when a whole committed prefix may retire.
  void retire_through(std::uint64_t sequence) noexcept;
  bool close() noexcept;
  bool expire() noexcept;
  Result<std::uint64_t> reserve_reconciliation(std::size_t max_updates);
  bool reconciliation_reserved(std::uint64_t reservation) const noexcept;
  std::uint64_t watermark() const noexcept { return watermark_; }
  std::uint64_t earliest_sequence() const noexcept;
  std::size_t retained_bytes() const noexcept { return bytes_; }
  std::size_t retained_events() const noexcept;
  std::size_t subscriber_count() const noexcept;
  bool healthy() const noexcept { return healthy_; }
  ObservationStreamState state() const noexcept { return state_; }
  const std::vector<EventGroup>& groups() const noexcept { return groups_; }
 private:
  struct StoredGroup { EventStorageClass storage; std::size_t bytes; };
  struct Subscriber { std::uint64_t id, cursor; std::size_t capacity; bool active; };
  struct Reconciliation { std::uint64_t id; std::size_t updates_remaining; bool closed; };
  ObservationLimits limits_;
  std::vector<EventGroup> groups_;
  std::vector<StoredGroup> group_metadata_;
  std::vector<Subscriber> subscribers_;
  std::vector<Reconciliation> reconciliations_;
  std::size_t bytes_{0}, control_{0}, emergency_{0}, reconciliation_{0}, telemetry_{0};
  std::size_t reserved_reconciliation_{0};
  std::uint64_t watermark_{0}, telemetry_dropped_{0}, next_subscriber_{1}, next_reconciliation_{1};
  std::uint64_t last_transition_{0};
  std::optional<RunId> run_;
  ObservationStreamState state_{ObservationStreamState::open};
  bool healthy_{true};
};

// Inspection records can be partial, unsupported or redacted. They are never
// submitted to EventStore or any command, resource, cleanup, or adapter interface.
struct InspectionGroup {
  std::string schema_version{"flamoris.event/1"};
  EventGroup group;
  std::size_t expected_events{0};
  bool redacted{false};
};
struct ReplayJobState { JobId job; JobState state; };
struct ReplaySnapshot {
  bool playback{true}, complete{true};
  std::uint64_t watermark{0};
  std::size_t applied_groups{0}, rejected_groups{0};
  std::vector<ReplayJobState> jobs;
  std::vector<IncompleteGroup> incomplete_groups;
};
class ReplayProjection {
 public:
  explicit ReplayProjection(std::size_t max_events = 4096, std::size_t max_jobs = 256);
  Result<void> apply(const InspectionGroup&);
  void note_gap() noexcept { snapshot_.complete = false; }
  const ReplaySnapshot& snapshot() const noexcept { return snapshot_; }
 private:
  std::size_t max_events_, max_jobs_, inspected_events_{0};
  std::optional<RunId> run_;
  ReplaySnapshot snapshot_;
};

bool known_event_kind(const std::string&) noexcept;
std::size_t event_group_storage_bytes(const EventGroup&) noexcept;

// Access is checked independently for every call against the supplied current
// policy. This view owns no controller, executor, adapter, or resource interface.
class AuthorizedObservation {
 public:
  AuthorizedObservation(const EventStore& store, std::string owner)
      : store_(store), owner_(std::move(owner)) {}
  Result<ObservationPage> read(const AuthorizationContext&, const PolicySnapshot&,
      std::uint64_t now_ms, std::uint64_t after, std::size_t max_events = 256) const;
  Result<ObservationPage> export_trace(const AuthorizationContext&, const PolicySnapshot&,
      std::uint64_t now_ms) const;
  Result<ReplaySnapshot> replay(const AuthorizationContext&, const PolicySnapshot&,
      std::uint64_t now_ms) const;
 private:
  const EventStore& store_;
  std::string owner_;
};

} // namespace flamoris::runtime
