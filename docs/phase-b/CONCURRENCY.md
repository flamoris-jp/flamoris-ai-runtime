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
mutable domain singleton is required. A DeliveryGate uses one narrow atomic
state for worker send versus control/worker close; its registry owns lifetime. The [ownership map](CPP_OWNERSHIP.md) specifies
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
| Keyed submission claim | Insert explicit scoped key + canonical request digest + reserved Run identity in SubmissionIndex | Different digest conflicts; same digest attaches to the pending decision |
| Unkeyed submission preparation | Allocate a fresh bounded PendingSubmissionId + reserved Run identity without inserting or querying SubmissionIndex | Independent admission/rejection; equal digests never attach |
| Run admission | Resolve the pending submission (and optional keyed claim) with one Run/root and all finite bookkeeping budgets | No runnable partial Run; notify keyed duplicate waiters before releasing failed claim |
| Resource reservation | All-or-none incremental vector in ResourceManager, advance ledger revision | No partial local grant; uncertain external receipts remain accounted |
| Dispatch/retry/resume | Final eligibility turn consumes prepared grants and budgets, commits running/attempt intent and a fenced outbound action | No useful work on a partially valid grant; retain queued resume payload or fail/clean up |
| Paid delivery close/send | DeliveryGate compare-exchange chooses absorbing no-send closure or one send claim after dispatch commit | Close wins: no provider call; send wins: retain liability, no local no-send proof |
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

Every validated request first has a bounded `PendingSubmission` owned by the
Runtime admission index, with fresh `PendingSubmissionId`, reserved RunId,
operation generation, deadline and response slot. This non-durable preparation
record is not an admitted Run. Pre-admission callbacks use its ID/generation
for both keyed and unkeyed requests; it is retired after decision delivery is
prepared and outstanding replies are fenced/routed to cleanup.

When the key is absent, do not query or insert `SubmissionIndex`, attach digest
waiters, or synthesize a null/empty/digest key. Each request independently passes
normal admission and budget checks and receives a fresh Run if accepted.
Disconnect does not cancel its admission; a client retry without a key is another
request and can produce another Run. The parser rejects present null/empty keys.
All pending records/response slots remain charged and bounded on either path.

For an explicit valid key, after canonical request validation and authentication,
claim scope is
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

Paid actions require the [one-shot DeliveryGate protocol](PAID_BUDGET_CONTRACT.md).
Allocate its stable identity and storage before arm; a matching durable receipt
alone cannot send. Commit `prepared -> dispatch_committed` with dispatch intent
before exposing the work item. A worker claims `dispatch_committed -> send_claimed`
once; cancel/revocation attempts absorbing closure before publishing stop. This
compare-exchange is the handoff race arbiter; the stop hint is not a no-send proof.
Arm loss/dispatch denial/enqueue failure close the same gate where possible and
route proof/unknown liability to accounting. Transfer gate ownership to cleanup
before destroying attempt/Run records. Late arm replies never reopen a gate.

## Pause barrier and pending results

A pause command reserves its bounded command/event budget, then performs the
whole preflight/gate-close/target-snapshot commit. Captured in-flight operations
must have an advertised quiescence path. Child registration and dispatch that
committed earlier are in the snapshot; later proposals cannot enter. A queued
outbound action already committed as running counts as in-flight, even if its
worker has not started it. Worker progress, completion, and cleanup can settle
captured targets while new useful dispatch is closed.

Each paused Job records a bounded set of causes (targeted command identities
and Run barrier generations). Removing one cause cannot resume a Job while
another remains. Timeout removes only the current barrier's causes; targeted pauses survive.
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

Event admission uses the following conservative **Phase B reservation profile v1**,
not an estimate of average logging. All variables are total maxima over the Run,
including retries, dynamic work, denied proposals and cleanup; they are never
reset by pause/resume. The compiler derives a finite maximum from the plan and
trusted contracts, or rejects it. Deployment can tighten the profile.

| Obligation | Maximum mandatory event slots reserved |
| --- | ---: |
| Run admission/activity/stop/finalization base | 32 |
| Each Job, `J` | 16 |
| Each attempted dispatch including denial/retry, `A` | 12 |
| Each suspension/wake/discard cycle, `S` | 8 |
| Each command admitted for control processing, `C`, including later rejection | 8 |
| Each join/race/group decision, `G` | 8 |
| Each bounded resource operation/acknowledgement episode, `R` | 8 |
| Each dynamic-fragment proposal including rejection, `D` | 8 |
| Each cleanup record, `Q`, with at most `U` post-terminal updates | `4 + 2*U`, including closure |

Thus `N = 32 + 16J + 12A + 8S + 8C + 8G + 8R + 8D + (4+2U)Q`.
Charge `C` before recording any requested/applied/rejected command events;
unsupported controls are not a free event-producing path. If normal command
capacity is exhausted, return a bounded pre-processing request error without a
new Run event. The Run/Job base allowances separately reserve one coalesced
terminating stop/cancel sequence per owner and internal deadline/cleanup progress;
ordinary controls cannot consume those slots or prevent an authorized terminal
cancel request from being processed. Repeated terminal cancel requests return
the same bounded receipt without another event sequence.
Fan-out barrier actions consume the corresponding Job/suspension/resource
budgets, not just the one command budget. Activity projection updates belong to
the causing obligation's slots. Partial acquisitions/transfer acknowledgements
consume their own bounded `R`; unknown operation maxima reject the capability.
Any extra mandatory diagnostic consumes its causing obligation's allowance;
an implementation needing more must revise this reviewed catalog, not drop it.

Each mandatory encoded envelope including payload is at most 4 KiB. Use bounded
logical references/summaries for larger results and participant sets. Internal
storage for a slot, its indexes and snapshot projection is charged at a maximum
8 KiB; Phase C must prove the chosen representation fits or reject/change the
profile before enabling it. Reserve `N * 8 KiB` in checked arithmetic, with a
default per-Run control-storage ceiling of 64 MiB and a process-wide configured
ceiling. Optional telemetry has a separate default 1 MiB per-Run pool. Results,
plans, worker payloads and current state are separately charged; this expression
does not hide them inside event memory. `U=4` by default; coalesce further updates
and reserve closure at transfer. No coefficient is an assertion that code exists.

The normal inbox defaults to 1,024 command envelopes of at most 2 KiB each;
large request/result buffers live in separately bounded owned storage, referenced
by opaque handles. A completion slot similarly holds at most 2 KiB metadata and
a handle to already charged output. Reserve one such slot per in-flight operation
and one coalesced stop slot per active Job, plus the cleanup records admitted by
`Q`; their aggregate storage must fit the process control budget before admission.
Duplicate waiter/response slots are separately charged before attachment. A full
normal inbox rejects new work and cannot steal any of these reserved slots.

A committed group has at most `N` slots; its atomic in-memory visibility uses a
group descriptor over reserved storage. Transport pages may explicitly mark an
incomplete group with finite total size; projections apply only a complete group,
otherwise return a gap/snapshot. Snapshot bytes are separately reserved at Run
admission from bounded Job/resource counts, with a default 1 MiB cap; status can
page Job details at the same watermark rather than grow an unbounded snapshot.
Unknown maxima, ceiling violations, and any arithmetic overflow reject admission.
Tests at every limit and one above it assert no operation begins without its
completion, mandatory transition, terminal and closure capacity.

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

Every observation is an owned value with a discriminated correlation ticket.
Pre-admission tickets carry incarnation, PendingSubmissionId and operation
generation, plus keyed-claim identity/generation when applicable, without a
fabricated Job/Attempt. Lifecycle tickets require
Run/Job/attempt/operation identity and relevant segment/suspension generations.
Cleanup tickets identify the independently retained cleanup/allocation/paid
operation and applicable backend/host epochs; expired Run references are optional
provenance. A callback captures a weak endpoint plus its immutable
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
3. Close all still-unclaimed delivery gates and retain registry/cleanup-owned
   gates, endpoints, resource/paid adapters and executor while workers acknowledge
   quiescence, release/containment, and ledger reconciliation. Claimed/unknown
   gates never become no-send proofs during shutdown.
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
