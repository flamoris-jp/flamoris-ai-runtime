#pragma once
#include "flamoris/runtime/lifecycle.hpp"
#include "flamoris/runtime/resources.hpp"
#include <optional>
#include <span>

namespace flamoris::runtime {
struct ReadyJob {
  JobId job;
  unsigned priority{};
  LogicalResourceId resource;
  ResourceVector requirements;
  TimePoint not_before{};
};
struct SchedulingChoice {
  JobId job;
  unsigned effective_priority{};
  std::uint64_t readiness_sequence{};
  bool protected_from_bypass{};
  Deadline acquisition_deadline{Deadline::at(TimePoint{})};
};
// Advisory ordering over Job IDs. No ownership of Continuations or leases;
// the composition root rechecks policy/ledger and commits controller dispatch.
class Scheduler {
 public:
  explicit Scheduler(std::size_t capacity = 1024);
  Result<void> ready(ReadyJob, const RunController&, TimePoint now);
  void remove(JobId) noexcept;
  Result<std::optional<SchedulingChoice>> select(std::span<RunController* const>, TimePoint now);
  std::size_t size() const noexcept { return ready_.size(); }
 private:
  struct Entry { ReadyJob value; TimePoint eligible_since; std::uint64_t sequence; };
  std::size_t capacity_;
  std::uint64_t next_sequence_{1};
  std::vector<Entry> ready_;
};
} // namespace flamoris::runtime
