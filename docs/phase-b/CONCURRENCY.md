# Control commits, concurrency, and lifetime

**Phase B proposal for review; no executor, scheduler, or tests exist yet.**
This implements the semantics of [State Machines](../STATE_MACHINES.md),
[Execution Model](../EXECUTION_MODEL.md), and [Event Model](../EVENT_MODEL.md).
The proposed types are responsibilities, not production declarations or a stable ABI.

## One control executor, independent workers

The baseline chooses one `ControlExecutor` thread per `RuntimeInstance`. It owns
all Run controllers, Job records, the Scheduler, ResourceManager ledger,
SubmissionIndex, current capability/policy projections, timer heap, and retained
observation records. Each Run therefore has a serialized control order. Shared
resource admission across Runs is atomic on the same executor, with a separate
ledger revision. This implementation convenience creates no public cross-Run
event clock. Per-Run parallel strands are a later measured optimization.

| Execution domain | Owns mutable state | Permitted interaction |
| --- | --- | --- |
| Control executor | Lifecycle, dependency/budget decisions, ledger, observation commits | Bounded in-memory work only; never block on I/O, inference, subscriber, or worker join |
| Backend worker | Its contexts, model handles, sampler/RNG, native allocation operations | One in-flight segment per context; immutable commands in, bounded observations out |
| Adapter workers | One bounded invocation and transport buffers per attempt | No direct access to Run/Job/controller; separate dispatch and outcome evidence |
| Validation workers | Bounded IR parsing/compilation over immutable snapshots | Return a candidate plan or typed rejection; recheck pins and policy at admission |
| Transport/observer workers | Authentication, bounded serialization and client buffers | Submit commands; receive authorized immutable views; never execute an event |

The inbox uses an ordinary mutex and condition variable over bounded queues.
The control thread releases the inbox mutex before processing. Worker queues
use the same simple pattern; no callbacks execute while a queue mutex is held.
`std::jthread`/stop requests express cooperative thread shutdown, not evidence
that native inference has stopped. No coroutine framework, lock-free queue, or
mutable singleton is required. The [ownership map](CPP_OWNERSHIP.md) specifies
destruction responsibility; [backend contracts](BACKEND_CONTRACT.md) specify
actual quiescence.

## Prepare, commit, deliver

Every control turn has three parts:

1. **Prepare:** validate a bounded command/observation, read current monotonic
   time, check identity/generations, and construct a bounded `CommitBatch` with
   proposed record changes, ordered events, and outbound actions. Reserve all
   needed storage before mutating authoritative state. Expensive parsing,
   policy I/O, provider calls, and allocation happen outside this turn.
2. **Commit:** recheck guards, then apply the prepared no-throw record updates,
   ledger conversions where applicable, event group, and snapshot watermark in
   one executor turn. Assign `seq` only here. No reader can observe half a
   group. If preparation fails, do not publish any part of the proposed change.
3. **Deliver:** place already reserved worker/response messages in their queues
   and publish immutable observer views. No observer acknowledgement is needed.
   A failed worker handoff returns a typed observation and unwinds through
   normal lifecycle rules; it never rolls back already visible history.

The first implementation uses preallocated bounded record slots and move/swap
operations in the commit section; it does not introduce a general allocator
architecture. No user/backend/plugin code, serialization, unbounded container
growth, or exception-producing native calls run inside commitment. Exhaustion
before admission rejects the request. An invariant failure inside commit closes
new admission and follows the fail-closed process containment policy; it cannot
claim the interrupted commit succeeded or continue using suspect records.

Resource requests that need external authority are prepared asynchronously.
Their replies are receipts, not permission to run independently. A final turn
validates the complete vector and all current fences before activating a lease.
No controller waits holding a mutex across an external operation. See
[Resource/host](RESOURCE_HOST_CONTRACT.md) and [paid budget](PAID_BUDGET_CONTRACT.md).

## Linearization points

| Operation | Single authoritative point | Rejected/stale consequence |
| --- | --- | --- |
| Submission claim | Insert scoped key + canonical request digest + reserved Run identity in SubmissionIndex | Different digest conflicts; same digest attaches to the pending decision |
| Run admission | Resolve that claim with one Run/root and all finite bookkeeping budgets | No runnable partial Run; notify duplicate waiters before releasing failed claim |
| Resource reservation | All-or-none incremental vector in ResourceManager, advance ledger revision | No partial local grant; uncertain external receipts remain accounted |
| Dispatch/retry/resume | Final eligibility turn consumes prepared grants and budgets, commits running/attempt intent and a fenced outbound action | No useful work on a partially valid grant; retain queued resume payload or fail/clean up |
| Yield | Safe-point evidence accepted; commit Continuation and waiting/paused state together | No resumable state fabricated from an unfinished native call |
| Wake | Consume owner/suspension generation into same Job's PendingResume and queue | Duplicate wake cannot consume again |
| Run pause | Validate every target, close both gates, assign barrier generation and capture target set together | Unsupported target rejects without changing flags/gates |
| Stop/completion | First eligible stop cause or valid completion selects cancelling/finalizing | Later result cannot override selected intent |
| Race winner | Commit one accepted terminal candidate and winner before loser stop/readiness | Rejected candidates never win; winner immutable |
| Terminal | Subtree terminal plus acknowledged release or valid containment transfer and reserved post-terminal records | Remain finalizing if obligations cannot be proven |
| Reconciliation | Ledger update first; authorized Run observation group references its revision if still retained | Old stream is never reopened; old allocation cannot free replacement |

Backend completion batches are ordered by the fixed participant order when they
are deliberately submitted as one race batch. Separately dequeued messages use
the Run commit order; backend wall timestamps cannot override that order.

## Submission claims and bounded duplicates

After canonical request validation and authentication, claim scope is
`(subject, tenant, request_kind, idempotency_key)`. Its record contains digest,
claim generation, reserved Run identity, pending/admitted decision, finite
deadline, and bounded response slots. A reserved ID is not an admitted Run and
produces no lifecycle events. A second equal request cannot compile/dispatch
another owner; a differing digest conflicts even while pending.

Admission may await resource/policy/budget evidence without blocking the
executor. On success, all attached response slots receive the same immutable
Run decision. On pre-Run rejection, fill all attached slots with the same
immutable rejection, detach them to response delivery, and only then release
the claim. Notification means making the decision available in a local response
slot, not waiting for a disconnected network client. A new caller can then
attempt fresh admission. Claim generation fences delayed owner replies.

The initiating transport disconnect does not cancel the owner decision or a
Run. A waiter disconnect releases only that response slot. Limits on pending
claims, duplicate waiters, and response bytes are checked before attachment;
overload returns a bounded request error and does not evict the owner. Admitted
claims remain associated with their Run for the advertised process-lifetime
retention window, including failure/unknown outcomes. Advertise the expiration;
after it or restart, absence cannot mean no previous operation occurred.

## Final dispatch and authorization races

Scheduler readiness is advisory. The dispatch turn checks Run/Job state, open
pause/child gates, monotonic deadline, retry eligibility, current policy revision
and concrete scopes, capability pins, preserved state, host/backend generation,
complete resource vector, event/result/child/attempt budgets, and required
durable paid receipt. A grant cannot outlive its recorded freshness/deadline.
Pure work is also authorized. PendingResume transfers to the active machine
only on successful dispatch; temporary capacity failure leaves it accounted.

All local policy/registry updates enter the control executor. An update committed
before dispatch denies/restricts it; an update after dispatch asks active work
to stop. This is not distributed atomic authorization. Snapshot fetch or remote
policy evaluation cannot be treated as indefinitely current permission.

For effectful handoff, the outbound action contains one-use dispatch identity,
the admitted concrete-input digest, effect decisions, grants, and expiry. The
adapter validates its configured authority and effect-specific receipt before
provider handoff. A stop flag is an atomic cooperative hint that may suppress an
action still queued, but it does not undo an earlier dispatch commit or prove
that a raced external send did not happen. Record `not_dispatched` only from
adapter proof; otherwise retain unknown outcome/liability. Adapter handoff and
provider acceptance are not falsely folded into the local commit.

## Pause barrier and pending results

A pause command reserves its bounded command/event budget, then performs the
whole preflight/gate-close/target-snapshot commit. Captured in-flight operations
must have an advertised quiescence path. Child registration and dispatch that
committed earlier are in the snapshot; later proposals cannot enter. A queued
outbound action already committed as running counts as in-flight, even if its
worker has not started it. Worker progress, completion, and cleanup can settle
captured targets while new useful dispatch is closed.

Each paused Job records the cause (targeted command or Run barrier generation).
Timeout removes only the current barrier's causes; targeted pauses survive.
Child results satisfy a paused wait condition but do not queue its owner.
Resume clears only the named cause, revalidates permission, and moves eligible
state through Continuation -> PendingResume -> active machine. A cancelled or
deadline-expired owner cannot be reopened. These transfers never decrement
resident bytes or manufacture a new Job/attempt identity.

## Clocks, readiness, and fairness

Inject `MonotonicClock` and a timer queue. Tests advance a fake clock without
sleep. Wall time is informational. At `now >= deadline`, useful dispatch and
first outcome acceptance fail even if a timer message has not arrived. Once
finalizing freezes an intent, use the separate cleanup deadline; workload
expiry and a late cancel cannot rewrite that intent.

The proposed default scheduler profile has four priority classes, 0 (lowest)
through 3. For an eligible queued Job, effective priority is
`min(3, base_priority + floor(eligible_wait / 1 second))`; choose highest then
oldest monotonic readiness sequence, then stable Job ID. After 3 seconds of
eligible wait, mark an older conflicting Job protected: stop granting capacity
to younger conflicting work and attempt bounded drain/reclaim. Do not preempt
required state or cross dependencies/effect order. If its own retained parent
state makes progress impossible, apply offload-or-resource-deadlock rules.

Resource acquisition defaults to a maximum 5-second attempt, further bounded
by Job/Run deadline and configured host/adapter contract. Requeue only when the
failure is explicitly transient and original budgets allow it; age is not reset
by unsuccessful capacity probes. Already running work is not magically preempted,
so fairness promises bounded bypass prevention, not guaranteed execution time.
Limits are finite validated deployment settings; the profile/revision and any
semantic per-Run limits are recorded in provenance. Changing a setting never
extends an already admitted deadline or revives a stopped Job.

Process no more than 64 bounded commands per turn before checking due timers,
reserved stop/cleanup signals, and other ready Run mailboxes. Deterministic
round-robin among ready Runs prevents a token producer monopolizing control.
Each individual command also has bounded graph/participant/output work; large
preparation is split outside the executor and revalidated before commit.

## Reserved callback and event capacity

Normal submission/telemetry queues are separate from mandatory completion,
stop, and cleanup capacity. Before issuing each worker/adapter operation,
reserve one completion slot and the bounded state/event payload it can return.
A producer writes that slot at most once and signals readiness; repeated status
updates coalesce in a separate bounded slot. Stop requests coalesce per active
operation with a monotonic generation. Duplicates cannot fill an unbounded queue.
The executor drains a slot before issuing the next segment. Failed telemetry
enqueue increments a bounded loss counter; it cannot suppress completion.

Event admission uses a checked, finite worst-case accounting expression, not
an estimate of average logging. For each admitted plan compute maximum root and
dynamic Jobs, attempts, suspensions, control requests, groups, resource operations,
and cleanup records from its finite limits. The Phase C event catalog assigns
each operation kind a fixed maximum mandatory event count and payload byte cap;
reserve the sum of those products plus terminal/closure capacity before the
operation can start. Unknown maxima or integer overflow reject admission.
Actual optional token/debug traffic has a separate finite byte/count budget.
Resource bounds include this storage, not only model memory.

Whole groups may expire after active bookkeeping is no longer needed; observers
get an explicit gap and current watermark. An active cleanup record cannot be
evicted to create fictional free capacity. Quarantine transfer reserves a finite
update allowance and one closure group before terminal. Updates beyond that
allowance coalesce into authoritative reconciliation state for the closure;
ledger progress never waits on a subscriber or exhausted telemetry space.

`reconciliation.closed` ends only the advertised observation window. At that
commit freeze the final known/unknown summary, then expire the stream by its
count/byte/time rules. Later matching evidence still changes the resource or
paid ledger without appending to the expired Run. Terminal status/result intent
was already immutable. No unbounded per-Run tombstone is kept to receive callbacks.

## Callback fences and destruction

Every observation is an owned value carrying runtime incarnation, Run/Job ID,
attempt ID, operation ID, relevant segment/suspension generation, and backend,
host, allocation epochs. A callback captures a weak endpoint plus its immutable
ticket, never a raw Run/controller pointer. Endpoint locking grants access only
to a bounded delivery slot; the callback cannot dereference lifecycle records.
Native buffers referenced by an in-flight call are owned by its worker context
until quiescence is proven. A discarded observation does not release buffers.

Validate lifecycle freshness and reconciliation identity separately. A late
completion cannot wake an old suspension, but a valid release/settlement proof
for an old cleanup record must still be processed. Unknown IDs are bounded stale
responses; address reuse never validates an allocation. Counters/epochs do not
wrap: exhaustion stops new identity allocation and requires a new incarnation.

Orderly shutdown is:

1. Close new admission and activation intents; publish draining status; freeze
   the configured drain deadline. Continue control/cleanup processing.
2. Drain eligible admitted work only within that deadline, then issue stop to
   remaining Jobs; invalidate resume paths and stop new segments.
3. Keep delivery endpoints, resource/paid adapters and executor alive while
   workers acknowledge quiescence, release/containment, and ledger reconciliation.
4. When no callbacks/native accesses can remain, stop/join workers outside the
   control thread, drain their final observations, and close their endpoints.
5. Publish remaining bounded closure records, close observers and timer sources,
   then destroy controllers, ledger and executor on their designated owners.

A native call that misses cleanup time cannot be detached and freed. If safe
containment is impossible, keep its memory/context and report stuck state to
the configured supervisor; do not join it on the control thread or fabricate
terminal events. Process termination loses non-durable Runs; restart uses a new
incarnation and requires host/budget reconciliation. No destructor sends a paid
request, resets a host device, or restores state from replay.

## Deterministic test hooks

`ManualControlExecutor` advances one prepared turn at a time with the same
commit logic. Injection points precede claim, return of prepared receipts,
dispatch commitment, segment completion acceptance, pause preflight commitment,
quarantine transfer, and observation closure. Run both command orders, exact
deadline equality, duplicate/stale messages, and every failure before delivery.
Use bounded allocation-failure injection before commit and assert no half-group
or partial lifecycle update is visible. See the complete
[A01–A42 test map](ACCEPTANCE_TEST_MAP.md); a future stress/ThreadSanitizer check
supplements these deterministic decisions and does not replace them.
