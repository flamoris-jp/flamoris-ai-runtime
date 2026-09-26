# Failure Model

## Status and authority

**Phase A architecture contract; no Runtime behavior is implemented.**

This document defines failure classification, propagation, and reconciliation.
The [State Machines](STATE_MACHINES.md) own lifecycle transitions, the
[Execution Model](EXECUTION_MODEL.md) owns parent/child coordination, and the
[Resource Model](RESOURCE_MODEL.md) owns resource release and quarantine.
Dispatch follows [Authorization Model](AUTHORIZATION_MODEL.md); observable
ordering follows [Event Model](EVENT_MODEL.md).
An error is evidence consumed by those authorities, never a second lifecycle.

Failures are bounded outcomes of a Run, Job, or request. A provider exception,
lost transport response, or cleanup timeout must not implicitly become a retry,
successful cancellation, resource release, or permission to continue.

## Structured error contract

Expose machine-readable classification independently of a short safe message.
The conceptual envelope contains:

| Field | Meaning |
| --- | --- |
| `code` / `category` | Stable Runtime-owned classification below; no provider exception names as the public contract. |
| `reason` | Optional bounded Runtime-owned refinement, such as `resource_deadlock` under `resource_unavailable`. |
| `message` | Bounded, sanitized description suitable for the authorized caller. |
| `run_id`, `job_id`, `attempt_id` | Applicable Runtime correlations; absent when admission never created them. |
| `stage` | Validation, admission, dispatch, execution, result validation, or cleanup. |
| `cause_codes` | Bounded sanitized causal chain; references may identify authorized child Jobs. |
| `external_outcome` | `not_dispatched`, `confirmed_success`, `confirmed_failure`, `unknown`, or not applicable. |
| `retry_disposition` | `prohibited`, `policy_eligible`, or `reconciliation_required`; never an instruction to automatically retry. |
| `diagnostic_ref` | Optional access-controlled reference to bounded diagnostic evidence. |

Envelope serialization/versioning is a Phase B decision. The semantic fields
must survive transport adapters without exposing raw upstream bodies, stack
traces, credentials, private endpoints, host paths, or unbounded input/output.
Validation paths use logical field/node identifiers. Sanitization also applies
to events and nested causes; hiding details only from the outer message fails.

## Failure taxonomy

| Category | Representative stable codes | Required handling |
| --- | --- | --- |
| Input/plan | `invalid_request`, `invalid_workflow`, `invalid_reference`, `unknown_capability`, `plan_stale` | Reject before admission/dispatch when detectable; no silent coercion. |
| Policy | `permission_denied`, `budget_exceeded` | Stop the denied dispatch; reauthorization cannot retroactively permit it. |
| Availability | `unsupported_model`, `capability_unavailable`, `backend_unavailable`, `resource_unavailable` | Distinguish unsupported capability from temporary capacity; neither permits fallback with different semantics. |
| Execution | `backend_failure`, `upstream_failure`, `state_unavailable` | Stop affected work; preserve sanitized cause and known external outcome. |
| Contract | `invalid_result`, `result_too_large` | Reject unsafe output before dependency binding or inference injection. |
| Deadline | `job_timeout`, `run_timeout` | Stop admission/dispatch in scope and initiate bounded cleanup; not successful cancellation. |
| Uncertainty | `outcome_unknown` | No proof of remote outcome; reconcile without redispatching. |
| Containment | `cleanup_failed`, `cleanup_timeout` | Retain accounting/quarantine, inhibit unsafe reuse, and report residual ownership. |
| Internal | `internal_error`, `invariant_violation` | Fail closed; isolate affected execution and expose no internal payload. |

`job_cancelled` / `run_cancelled`, when returned by a result API, describe the
already committed cancellation outcome; they do not prove upstream rollback.
Unsupported control requests fail that request without corrupting the Job.
Unknown provider errors map to a safe Runtime code, not an invented success.

## Timeout, cancellation, and uncertain effects

The Kernel orders stop requests and completion using the canonical transition
rules. The first committed stop cause is retained. Once finalization selects
its target, late cancellation/completion does not overwrite that target.

Timeout applies across queueing, execution, waiting, and pause. Child deadlines
cannot extend their ancestors, and resume/retry must not reset elapsed budgets.
A caller transport timeout/disconnect alone is not a Run timeout: status may
still be queried, and transport retry follows submission deduplication rules.

Cancellation acknowledges a request first; it is not terminal on receipt.
Stop new dispatch, request cooperative stops, prevent stale callbacks from
mutating Job state, and settle descendants/resources before terminal commit.
Do not claim a wedged native backend has stopped merely because a timer fired.
Fencing a callback alone does not isolate an in-process writer or free memory.

For external work, distinguish rejection before dispatch, confirmed completion,
and a dispatched request whose response/outcome cannot be established. Provider
acceptance followed by response loss is `unknown`, even when no result arrived.
An unacknowledged remote cancellation is not proof that the operation stopped.
Confirmed provider failure may still include partial effects; retain that
evidence separately and never interpret failure as proof of rollback.

A deadline produces failed status with `job_timeout` / `run_timeout`; attach
`external_outcome: unknown` when needed. A cancellation whose remote outcome
remains unknown produces failed status with `outcome_unknown`, not cancelled.
Confirmed completion after a committed cancellation is recorded as effect
evidence without promoting the Job to succeeded. Cancellation never rolls back
completed writes, billed work, or provider-owned artifacts.
Once stopping is confirmed, cancelled may therefore carry `confirmed_success`
effect evidence; it describes the local cancellation, not remote rollback.

## Retry and idempotency

Default to no automatic retry. A transient-looking error is insufficient.
Eligibility requires explicit bounded policy, remaining deadline/budget,
current authorization, and the registered adapter's verified retry contract.
Effect labels alone do not establish idempotency or refund paid quota.

For one semantic operation, a provider-supported idempotency key must remain
stable across eligible attempts and bind to the same canonical input and
principal. A changed payload is a different operation. A local request ID or
MCP transport ID does not prove provider deduplication; expiry/scoping matters.
Without sufficient provider guarantees, uncertain non-idempotent work requires
reconciliation or an explicitly authorized new operation, never blind repeat.

Run-submission deduplication and provider-operation idempotency are separate.
Within its advertised retention scope, a repeated submission key with identical
input returns the existing Run; different input is rejected. After retention
expires or a non-durable process restarts, absence is not evidence that no work
occurred. Do not promise exactly-once execution across that boundary.

A retry attempt must not overlap an earlier still-running/unknown attempt.
Record attempt identity, trigger, budget charge, and dispatch evidence. Any
preterminal retry follows the Execution Model rather than bypassing lifecycle
transitions: `running -> queued` with retry backoff, unchanged Job identity and
deadline, a new attempt ID, and cumulative budget. Never retry from cancelling
or finalizing, and do not represent backoff as a Continuation.
Re-execution after terminal creates new Run/Job identities with
provenance to the original; it never reopens or silently mutates a terminal Job.

## Propagation and result boundaries

For a required `await` / `all_success`, child failure fails the parent and makes
its Continuation non-resumable. Recovery uses an explicitly declared `all_settled`
collection and typed failure binding; it is never ordinary success data. Model
output cannot invent a new recovery branch or authorization.

Fail-fast join requests sibling cancellation after the first required failure,
then drains or contains descendants before parent finalization. It does not
report them all as cancelled on the first request. All-settled join waits for
all child outcomes, then returns a bounded outcome collection; a failed child
does not implicitly fail that collecting parent. Its own deadline still wins.

Race follows its declared acceptance and loser policy. A failed candidate is
not a winner; no accepted candidate produces the declared no-winner failure.
Selecting a winner does not erase uncertain loser effects or cleanup debt.
Unstarted dependents are never supplied absent/invalid output as if successful.

Validate provider results against size/schema/encoding/reference limits before
commit and before inference injection. Prefer bounded streams/handles; enforce
limits while receiving, not only after allocating the whole response. Invalid
or oversized output may follow a completed paid/write operation, so output
rejection must preserve effect evidence and must not silently trigger a retry.
Provider-owned artifacts remain under their service's retention/deletion policy.

## Cleanup, shutdown, and crash

Graceful shutdown stops new admission, fixes the configured drain deadline,
then requests cancellation of remaining work. Cleanup has a separately bounded
deadline so an expired Run does not either skip cleanup or wait forever.
Continuation invalidation prevents future resume immediately; disposal may
transfer retained state to Resource Manager cleanup ownership until confirmed
free. Removing a Continuation is not evidence of VRAM/RAM reclamation.

Before terminal publication, account for every child, lease, and footprint as
settled or contained under explicit cleanup ownership. Quarantined capacity
remains unavailable. A device failure may require GPU Node Manager intervention
under its separate host authority; the Kernel cannot grant itself that power.
If safe local containment is impossible, report the stuck execution and let
the configured process supervisor stop it; do not fabricate terminal cleanup.

Cleanup errors are recorded alongside the primary failure without replacing
the first stop cause. If execution selected success but cleanup finds residue,
report that residue/quarantine separately and do not imply resources are free.
Bounded containment permits lifecycle completion, not unlimited resource reuse.

The initial Runtime is non-durable. A process crash can leave the final event
missing and external operations running; it cannot manufacture cancelled/failed
records for Jobs whose final transition was never committed. Restart creates a
new process incarnation and does not automatically resume Jobs or Continuations.
Retained traces are observations, not checkpoints, deduplication ledgers, or
permission to reconstruct execution. A separate durable paid-budget authority
retains unresolved attempt liability across restart; affected paid admission
remains blocked until reconciliation or conservative bounded liability accounting
certifies remaining budget. This does not restore Jobs or prove external execution.
Trace replay never executes cleanup/calls.

## Scenario obligations

Names below describe semantic events; the Event Model owns their encoding and
ordering. Each terminal event follows the committed transition and cleanup or
containment record. No required event relies on an observer remaining connected.

| Trigger | Job / parent outcome | Continuation | Resources | Event/effect evidence |
| --- | --- | --- | --- | --- |
| Invalid plan or admission denial | Reject request; no dispatch | None created | Release admission reservations | Rejection with safe code; no fabricated Job lifecycle |
| Required MCP child timeout | Child failed/timeout; unhandled parent fails | Invalidate; no result injection | Release confirmed local work, retain/quarantine residue | Stop, child outcome, propagation, cleanup, terminal |
| MCP accepted; response lost | Failed/unknown after bounded reconciliation | No success binding; explicit error recovery only | Local adapter settles; remote outcome remains tracked | Dispatch evidence and unknown outcome, no redispatch |
| Cancel during inference pause | Stop/cleanup, then cancelled when verified | Invalidate and transfer retained-state cleanup | Footprint charged until actual release | Cancel request, invalidation, release/containment, terminal |
| Run deadline during all-settled join | Stop parent and outstanding children; failed/timeout | Invalidate outstanding resume | Drain or quarantine each child | Deadline precedes propagated stops; preserve settled results |
| Race winner with non-stopping loser | Winner stays selected; parent follows cleanup policy | Only declared winner binding | Loser capacity stays charged/contained | Winner plus separate loser unknown/cleanup evidence |
| Provider result oversized after write | Child failed/result-too-large; normal propagation | Never inject rejected payload | Local buffers bounded; remote asset unchanged | Result rejection and confirmed/unknown write evidence |
| Backend cannot stop before cleanup deadline | Stay unresolved until safe containment; no false stop | Never resume | Quarantine or supervisor termination | Cleanup timeout; no fictitious release/completion |
| Process crashes after dispatch | No invented terminal outcome; restart cannot resume | Lost/nonrecoverable | Rediscover through proper resource authority | Last trace may end at dispatch; outcome needs reconciliation |

## Reconciliation and review gate

Reconciliation queries an approved provider status surface without repeating
the original operation. Give it its own deadline, authorization, and budget;
if the provider cannot answer, retain `unknown`. Polling is not free authority.
After terminal commit, attach bounded reconciliation evidence to the operation
record; never change the Job's terminal state, inject a late result, restart
descendants, or retract already published events. Expose both facts to callers.

Phase B must map these contracts to typed errors, clocks, attempt records,
containment interfaces, and deterministic tests, including response-loss,
cancel/completion ordering, cleanup failure, and process-restart uncertainty.
