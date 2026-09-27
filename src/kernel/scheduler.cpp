#include "flamoris/runtime/scheduler.hpp"
#include <algorithm>
#include <limits>

namespace flamoris::runtime {
namespace {
bool conflicts(const ReadyJob& left, const ReadyJob& right) noexcept {
  if (left.resource != right.resource) return false;
  for (std::size_t i = 0; i < left.requirements.values.size(); ++i)
    if (left.requirements.values[i] && right.requirements.values[i]) return true;
  return false;
}
}
Scheduler::Scheduler(std::size_t capacity) : capacity_(capacity) { ready_.reserve(capacity); }
Result<void> Scheduler::ready(ReadyJob value, const RunController& controller, TimePoint now) {
  auto job = controller.job(value.job);
  if (!job || job.value().state != JobState::queued || value.priority > 3 ||
      job.value().deadline.expired(now)) return Result<void>::failure(ErrorEnvelope::make(ErrorCode::invalid_request));
  for (const auto& entry : ready_) if (entry.value.job == value.job) return Result<void>::success();
  if (ready_.size() == capacity_ || next_sequence_ == std::numeric_limits<std::uint64_t>::max())
    return Result<void>::failure(ErrorEnvelope::make(ErrorCode::budget_exceeded));
  const auto eligible = std::max(now,value.not_before);
  ready_.push_back({std::move(value), eligible, next_sequence_++}); return Result<void>::success();
}
void Scheduler::remove(JobId id) noexcept { std::erase_if(ready_, [id](const auto& entry) { return entry.value.job == id; }); }
Result<std::optional<SchedulingChoice>> Scheduler::select(std::span<RunController* const> controllers, TimePoint now) {
  using namespace std::chrono_literals;
  struct Candidate { const Entry* entry; Deadline deadline; unsigned priority; bool protected_job; };
  std::vector<Candidate> candidates; candidates.reserve(ready_.size());
  for (auto& entry : ready_) {
    RunController* controller = nullptr;
    for (auto* item : controllers) if (item && item->id() == entry.value.job.run()) { controller = item; break; }
    if (!controller) continue;
    auto snapshot = controller->job(entry.value.job);
    if (!snapshot || snapshot.value().state != JobState::queued || snapshot.value().pause_requested ||
        !controller->snapshot().dispatch_open || snapshot.value().deadline.expired(now)) continue;
    if (now < entry.value.not_before) { entry.eligible_since = entry.value.not_before; continue; }
    const auto age = now >= entry.eligible_since ? now - entry.eligible_since : TimePoint{};
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(age).count();
    const auto priority = std::min(3U, entry.value.priority + static_cast<unsigned>(std::min<std::int64_t>(seconds,3)));
    candidates.push_back({&entry,snapshot.value().deadline,priority,age >= 3s});
  }
  const Candidate* choice = nullptr;
  for (const auto& candidate : candidates) {
    bool bypasses = false;
    for (const auto& older : candidates)
      if (older.protected_job && older.entry->sequence < candidate.entry->sequence &&
          conflicts(older.entry->value,candidate.entry->value)) { bypasses = true; break; }
    if (bypasses) continue;
    if (!choice || candidate.priority > choice->priority ||
        (candidate.priority == choice->priority && candidate.entry->sequence < choice->entry->sequence) ||
        (candidate.priority == choice->priority && candidate.entry->sequence == choice->entry->sequence && candidate.entry->value.job < choice->entry->value.job))
      choice = &candidate;
  }
  if (!choice) return Result<std::optional<SchedulingChoice>>::success(std::nullopt);
  const auto acquisition = Deadline::after(now,5s);
  const auto bound = acquisition ? std::min(acquisition.value().time(),choice->deadline.time()) : choice->deadline.time();
  return Result<std::optional<SchedulingChoice>>::success(SchedulingChoice{choice->entry->value.job,
      choice->priority,choice->entry->sequence,choice->protected_job,Deadline::at(bound)});
}
} // namespace flamoris::runtime
