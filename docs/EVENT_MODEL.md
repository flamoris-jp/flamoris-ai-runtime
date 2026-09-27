# Events, Observation, and Trace Replay

## Status and authority

**Phase A architecture contract; no Event Bus, journal, or replay engine is implemented.**
This defines observable semantics; wire encoding and C++ representation belong to Phase B.
The owning Run/Job controllers and resource ledger commit state. Events report those
commits and adapter observations; receiving an event never independently mutates a Job.
Structured events are the primary observation contract, with human-readable logs derived
from them. Execution correctness does not depend on an attached observer.

## Envelope and identities

Each retained Run event has bounded structured fields:

| Field | Purpose |
| --- | --- |
| `schema_version`, `kind` | Versioned interpretation and event family |
| `runtime_instance_id`, `run_id`, `seq` | Restart boundary, owning Run and strictly increasing per-Run order |
| `event_id` | Stable deduplication identity within the runtime instance |
| `job_id`, `attempt_id` when applicable | Owning Job and concrete attempt; never inferred from display text |
| `transition_id`, `group_index`, `group_size` | Identify the complete contiguous event group for one state commit |
| `causation_id`, `command_id` when applicable | Link accepted command, parent action, or prior commit |
| `monotonic_offset`, `wall_time` | Relative timing and informational timestamp |
| `payload` | Kind-specific bounded, redacted data/references |

Resource/adapter acknowledgement payloads additionally identify operation and relevant
native-worker/host generation so stale acknowledgements cannot release new allocations or
complete a later attempt. A Continuation inspection ID is provenance, not an independent
Job or scheduling authority. Secrets, pointers and raw native state are not event fields.
Wall-clock order is never used to settle a race, deadline or cancellation.
Pre-admission rejection is a bounded request response; optional security audit records
use request correlation outside the Run stream and do not fabricate a Run/Job lifecycle.

## Commit order and visibility

Each Run has one logical serialized commit order. This is an architectural requirement,
not a decision to use one OS thread. A state transition and its ordered lifecycle event
group become visible atomically in memory. Sequence numbers are allocated at commitment;
concurrent callbacks cannot manufacture an alternate Job history.

Events in a group are contiguous and ordered. Observation APIs must preserve the group
boundary or identify an incomplete group explicitly; projection consumers apply a group
only when complete. A state snapshot includes its sequence watermark so reconnecting
clients can combine a current snapshot and subsequent events without double-applying work.
There is no total order across Runs. Shared resource decisions carry an independent
ledger revision; Run events refer to that revision rather than inventing a global clock.

Before accepting progress, the controller evaluates deadline/cancellation eligibility
under [EXECUTION_MODEL.md](EXECUTION_MODEL.md). Completion and cancellation commands are
serialized at their commit point. An accepted cancel request is distinct from its
application and eventual terminal outcome. Late callbacks may reconcile resource/cost
ledgers but cannot resurrect a terminal Job or replace a committed race winner.
A terminal state transition closes the workload lifecycle, not the Run observation
stream. Within bounded retention, authorized post-terminal reconciliation commits
append to that same Run stream with increasing `seq`, correlation to the original
operation/allocation/generation and the current resource or paid-ledger revision.
These commits update cleanup/outcome observations only; they cannot revise the
terminal state, result intent or prior events. The terminal event is not end-of-stream.

## Lifecycle and observation families

These names establish the architecture vocabulary, not a frozen wire API:

| Family | Required meaning |
| --- | --- |
| `run.state_changed`, `job.state_changed` | Committed old/new state, reason, and relevant result/error reference |
| `continuation.created` | Waiting/paused Job acquired owned resume state |
| `continuation.consumed` | Resume condition committed; payload transferred to the same Job's pending resume state |
| `continuation.discarded` | Resume abandoned; state cleanup tracked separately |
| `interrupt.requested`, `interrupt.applied`, `interrupt.rejected` | Separate acceptance, control-point application and rejection |
| `attempt.dispatch_committed`, `attempt.outcome` | Authorized adapter handoff intent and bounded confirmed/unknown outcome |
| `race.winner_selected`, `join.completed` | Controller decision and participant/result provenance |
| `resource.lease_acquired`, `resource.lease_released`, `resource.lease_revoked` | Execution permission changed; does not prove memory release |
| `resource.allocation_release_confirmed`, `resource.allocation_unknown` | Physical-accounting evidence or uncertainty with allocation/generation identity |
| `authorization.denied`, `budget.reserved`, `budget.settled` | Policy/budget decision metadata without secrets |
| `reconciliation.observed`, `reconciliation.closed` | Bounded post-terminal cleanup/attempt evidence or closure of its observation window; no lifecycle change |
| `inference.progress`, `token.generated`, `sampling.observed` | Optional bounded telemetry; not Job transition authority |

Legacy conceptual event names in overview examples can be rendered views of these
commits; implementations must not emit conflicting terminal authorities under aliases.
`attempt.dispatch_committed` means the Runtime decided to hand off; it is not proof the
provider received or completed an action. External observations are labeled with the
strength of available evidence.

## Representative lifecycle ordering

A yielding InferenceJob commits one group containing `continuation.created` then
`job.state_changed(running -> waiting)`. A resource lease release is a separately
acknowledged resource decision; a retained allocation remains accounted.
Child completion commits its own Job outcome before the parent consumes that result.
A successful wake commits `continuation.consumed` followed by
`job.state_changed(waiting -> queued)` with the same parent Job ID. Reauthorization and
resource acquisition precede a later `queued -> running` commit.

If cancellation wins before child completion, the parent enters `cancelling`, releases
its Continuation ownership through the required cleanup path, and rejects later wake.
A completion accepted first enters `finalizing` with immutable terminal intent; later
cancel cannot replace that outcome. Finalization waits for the defined child and
release-or-quarantine obligations before committing terminal status. Confirmed remote
success after accepted cancellation is recorded as an external outcome without
replacing the cancellation intent; an unknown outcome uses the failure rules rather
than claiming confirmed cancellation. [STATE_MACHINES.md](STATE_MACHINES.md) and
[FAILURE_MODEL.md](FAILURE_MODEL.md) define those conditions.

## Bounded storage and backpressure

Lifecycle/control records and optional telemetry have distinct bounded capacity.
Admission accounts for maximum Jobs, attempts, suspensions and accepted control commands
so mandatory transition records and cancellation/terminal cleanup capacity are reserved.
Transfer to quarantine reserves finite post-terminal reconciliation record capacity
before terminal publication, including a closure record; finite attempts/updates are
coalesced when possible. If capacity cannot be secured, the affected Run cannot
publish terminal via quarantine transfer. Repeated late callbacks cannot exhaust
unbounded control records or block authoritative ledger cleanup.
An operation that cannot reserve its required records cannot start. Repeated no-op
commands are rate-limited/coalesced; they cannot consume unbounded control capacity.

Slow/disconnected stream clients never block native-worker stop, resource release or lifecycle
commit. Optional token/debug telemetry may be sampled, coalesced or dropped according to
the advertised policy; every loss is represented by counters/ranges in the next available
bounded telemetry summary. Control records are never silently treated as dropped telemetry.
When normal capacity is exhausted, stop admitting new work; retain reserved cleanup
capacity and authoritative current state. Exact sizing and overflow tests belong to Phase B.

Post-terminal observation
remains available only within the advertised Run retention/window; before it closes,
the stream records final known reconciliation or an explicit still-unknown closure.
After that bound, late authoritative release/settlement still updates its resource ledger (or optional strict-cost external ledger), but cannot reopen an expired Run stream. Queries to an
expired Run report a gap/expiration rather than inventing a late lifecycle event.
The retained history is bounded by count/bytes/time. Whole committed groups may age out,
including control groups, once their active bookkeeping need is satisfied. A reader with
an expired cursor receives an explicit gap with earliest retained sequence and a current
snapshot/watermark. It must not reconstruct a fictional uninterrupted history.
Subscriber buffer overflow similarly reports a gap; it cannot mutate execution state.

## Journal and replay boundaries

The initial contract requires in-memory committed observation, not durable event sourcing.
Optional journal persistence may later retain committed groups for inspection; asynchronous
persistence can lose a suffix on crash. Partial groups are marked incomplete and excluded
from authoritative projection. Journal errors are observable and cannot turn a partially
persisted record into proof that an external effect did or did not occur.

Trace replay reads retained records into isolated observer/projection state only.
It cannot acquire resources, resume Jobs, call adapters, change registry/policy, or publish
replayed records onto the live command path. Replay preserves original identity/order and
marks the playback context. Missing/redacted/unsupported records make the projection
explicitly incomplete; replay does not infer hidden outcomes.

Re-execution is a separate newly admitted Run with fresh authority and provenance linking
the source Run. Retry is a live bounded attempt governed by
[AUTHORIZATION_MODEL.md](AUTHORIZATION_MODEL.md). Neither is an alias of replay.
Regression replay may check lifecycle, continuation, resource and effect ordering; exact
generated model text is not a deterministic architecture invariant.

Crash recovery, durable exactly-once dispatch, cross-restart continuation restoration,
and reconstruction of runnable work from an event log are outside the initial scope.
Runtime restart assigns a new instance identity. Old IDs/cursors do not imply live Jobs;
resource/provider reconciliation is required before uncertain capacity or outcomes can be
treated as resolved. A persisted trace alone cannot authorize restoration or resubmission.

## Privacy and access

Run status, event subscription, retained history, exports and replay each require current
current local-user authorization. Historical execution permission does not grant perpetual
trace access. Trace levels are capped by deployment policy and the caller's access scope.
Tokens/prompts, model-exposed reasoning channels and debug probes are opt-in sensitive
payloads with finite size/retention, not default lifecycle fields.

Secrets, credentials, unrestricted provider payloads, tensors and private topology are
removed before entering journal/subscriber buffers. Service-owned media/result handles
remain subject to their owner's access policy and may expire independently of the trace.
Redaction/retention gaps are distinguishable from nonexistent events without disclosing
the hidden payload. Terminal Run status does not mean cleanup/quarantine accounting has
finished; status and subsequent resource reconciliation expose that distinction.
