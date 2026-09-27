#pragma once
#include "flamoris/runtime/compiler.hpp"
#include "flamoris/runtime/lifecycle.hpp"
#include <memory>
#include <optional>
#include <span>

namespace flamoris::runtime {
struct WorkflowInvocation {
  JobId job;
  std::string node;
  std::optional<CapabilityPin> capability_pin;
  JsonValue input;
  ValueSchema output_schema;
  EffectSet effects{EffectSet::from_mask(1).value()};
  Deadline deadline{Deadline::at(TimePoint{})};
  bool coordinating{};
};
struct WorkflowAdvance {
  std::vector<WorkflowInvocation> invocations;
  std::optional<JsonValue> completed_output;
};
struct WorkflowOutcome {
  JobId job;
  std::optional<JsonValue> result;
  std::optional<ErrorEnvelope> failure;
};

// Owns only bounded immutable invocation data and dependency/group bookkeeping.
// RunController remains the only lifecycle committer, and every group is a Job.
class WorkflowMachine {
 public:
  static Result<std::unique_ptr<WorkflowMachine>> create(
      std::shared_ptr<const ExecutionPlan>, JsonValue::Object inputs,
      RunController&, MonotonicClock&);
  static Result<std::unique_ptr<WorkflowMachine>> create_at(
      JobId coordinating_owner, std::shared_ptr<const ExecutionPlan>,
      JsonValue::Object inputs, RunController&, MonotonicClock&);
  ~WorkflowMachine();
  WorkflowMachine(const WorkflowMachine&) = delete;
  WorkflowMachine& operator=(const WorkflowMachine&) = delete;
  bool is_coordinator(JobId) const noexcept;
  Result<WorkflowAdvance> advance(DispatchTicket);
  // Results are accepted only after their child controller reached terminal.
  Result<void> accept_result(JobId, JsonValue);
  Result<void> accept_failure(JobId, ErrorEnvelope);
  // Deliberately simultaneous observations use fixed declared participant order.
  Result<void> accept_batch(std::span<const WorkflowOutcome>);
  std::optional<WorkflowInvocation> invocation(JobId) const;
  std::uint64_t control_steps() const noexcept;
 private:
  struct Impl;
  explicit WorkflowMachine(std::unique_ptr<Impl>);
  std::unique_ptr<Impl> impl_;
};
} // namespace flamoris::runtime
