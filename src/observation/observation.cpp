#include "flamoris/runtime/observation.hpp"
#include "flamoris/runtime/authorization.hpp"
#include <algorithm>
#include <limits>

namespace flamoris::runtime {
namespace {
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
}
ReplayProjection::ReplayProjection(std::size_t max_events, std::size_t max_jobs)
    : max_events_(std::min<std::size_t>(max_events, 65536)), max_jobs_(std::min<std::size_t>(max_jobs, 65536)) {
  snapshot_.jobs.reserve(max_jobs_);
  snapshot_.incomplete_groups.reserve(max_events_);
}
Result<void> ReplayProjection::apply(const InspectionGroup& input) {
  const auto count = input.group.events.size();
  const auto inspection_charge = std::max<std::size_t>(count, 1);
  if (inspection_charge > max_events_ - inspected_events_) return Result<void>::failure(capacity());
  inspected_events_ += inspection_charge;
  const auto expected = input.expected_events ? input.expected_events : count;
  auto reject = [&]() {
    snapshot_.complete = false; ++snapshot_.rejected_groups;
    if (snapshot_.incomplete_groups.size() < max_events_)
      snapshot_.incomplete_groups.push_back({input.group.transition, input.group.first_sequence, expected, count});
    return Result<void>::success();
  };
  if (input.schema_version != "flamoris.event/1" || input.redacted || !count || expected != count)
    return reject();
  if (last_sequence(input.group) <= snapshot_.watermark) return Result<void>::success();
  const auto previous = input.group.first_sequence == 0 ? UINT64_MAX : input.group.first_sequence - 1;
  if (!valid_group(input.group, previous)) return reject();
  if (run_ && *run_ != input.group.events.front().job.run()) return reject();
  if (snapshot_.watermark && input.group.first_sequence <= snapshot_.watermark) return reject();
  if (input.group.first_sequence != snapshot_.watermark + 1) snapshot_.complete = false;
  try {
    // Prepare an isolated candidate so a malformed event never partially applies
    // a complete group, including contradictory terminal-state changes.
    auto candidate = snapshot_.jobs;
    for (const auto& event : input.group.events) {
      if (event.kind == "job.created") {
        auto job = std::find_if(candidate.begin(), candidate.end(), [&](const auto& j) { return j.job == event.job; });
        if (job != candidate.end()) return reject();
        if (candidate.size() == max_jobs_) return Result<void>::failure(capacity());
        candidate.push_back({event.job, JobState::created});
        continue;
      }
      if (event.kind != "job.state_changed" || !event.to) continue;
      auto job = std::find_if(candidate.begin(), candidate.end(), [&](const auto& j) { return j.job == event.job; });
      if (job == candidate.end()) {
        if (candidate.size() == max_jobs_) return Result<void>::failure(capacity());
        candidate.push_back({event.job, *event.to});
      } else {
        if ((is_terminal(job->state) && job->state != *event.to) ||
            (event.from && (*event.from != job->state ||
             !is_allowed_transition(*event.from, *event.to)))) return reject();
        job->state = *event.to;
      }
    }
    snapshot_.jobs.swap(candidate);
    run_ = input.group.events.front().job.run();
    snapshot_.watermark = last_sequence(input.group);
    ++snapshot_.applied_groups;
    return Result<void>::success();
  } catch (...) { return Result<void>::failure(internal()); }
}

Result<ObservationPage> AuthorizedObservation::read(const AuthorizationContext& context,
    const PolicySnapshot& policy, std::uint64_t now, std::uint64_t after, std::size_t max_events) const {
  auto access = AuthorizationGate{}.check_access(context, policy, owner_, AccessSurface::events, now);
  if (!access) return Result<ObservationPage>::failure(access.error());
  return store_.read(after, max_events);
}
Result<ObservationPage> AuthorizedObservation::export_trace(const AuthorizationContext& context,
    const PolicySnapshot& policy, std::uint64_t now) const {
  auto access = AuthorizationGate{}.check_access(context, policy, owner_, AccessSurface::export_data, now);
  if (!access) return Result<ObservationPage>::failure(access.error());
  return store_.read(0, 65536, 64 * 1024 * 1024);
}
Result<ReplaySnapshot> AuthorizedObservation::replay(const AuthorizationContext& context,
    const PolicySnapshot& policy, std::uint64_t now) const {
  auto access = AuthorizationGate{}.check_access(context, policy, owner_, AccessSurface::replay, now);
  if (!access) return Result<ReplaySnapshot>::failure(access.error());
  try {
    auto page = store_.read(0, 65536, 64 * 1024 * 1024);
    if (!page) return Result<ReplaySnapshot>::failure(page.error());
    ReplayProjection projection(65536, 65536);
    if (page.value().gap || page.value().incomplete_group || page.value().telemetry_dropped ||
        page.value().next_after != page.value().watermark) projection.note_gap();
    for (const auto& group : page.value().groups) {
      auto applied = projection.apply(InspectionGroup{"flamoris.event/1", group, group.events.size(), false});
      if (!applied) return Result<ReplaySnapshot>::failure(applied.error());
    }
    return Result<ReplaySnapshot>::success(projection.snapshot());
  } catch (...) { return Result<ReplaySnapshot>::failure(internal()); }
}
} // namespace flamoris::runtime
