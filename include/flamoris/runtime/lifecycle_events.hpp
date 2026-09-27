#pragma once

#include "flamoris/runtime/error.hpp"
#include "flamoris/runtime/ids.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace flamoris::runtime {

enum class JobState { created, queued, running, waiting, paused, cancelling,
                      finalizing, succeeded, failed, cancelled };
enum class RunActivity { created, queued, running, waiting, paused, cancelling,
                         finalizing, succeeded, failed, cancelled };

[[nodiscard]] bool is_terminal(JobState state) noexcept;
[[nodiscard]] bool is_allowed_transition(JobState from, JobState to) noexcept;
[[nodiscard]] const char* state_name(JobState state) noexcept;

// These are observations, never executable messages. Only a controller assigns
// sequence numbers. Groups are immutable after publication by their sole owner.
struct LifecycleEvent {
  std::uint64_t sequence{};
  std::string kind;
  JobId job;
  std::optional<JobState> from;
  std::optional<JobState> to;
  std::uint64_t generation{};
  std::optional<ErrorCode> error;
  std::uint64_t operation{};
  std::uint64_t ledger_revision{};
  std::optional<ExternalOutcome> external_outcome;
};

struct EventGroup {
  std::uint64_t transition{};
  std::uint64_t first_sequence{};
  std::vector<LifecycleEvent> events;
};

} // namespace flamoris::runtime
