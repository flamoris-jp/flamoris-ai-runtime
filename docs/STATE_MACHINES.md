# State Machines

## Status and governing rules

**Phase A architecture contract; planned behavior only.** These are semantic
states, not C++ enums or a frozen API schema. All unspecified transitions are
rejected deterministically. Terminal states are immutable. Machines, resources,
and control groups never gain an independent Job lifecycle.

Each accepted Run has a serialized control-commit order. A transition and its
bounded event group become visible together; subscribers may miss delivery but
cannot cause the transition. A snapshot identifies its committed sequence.
Cross-Run resources have an atomic admission boundary, not an invented global
event clock. Monotonic deadline checks guard useful progress and first outcome
acceptance: an observation arriving at or after the deadline cannot defeat
timeout simply because a timer callback was delayed. Once finalizing has frozen
terminal intent, only the separate cleanup deadline applies; workload expiry
cannot overwrite a completion already accepted before that deadline.

## Job transition table

| From | Trigger / guard | To | Required action |
| --- | --- | --- | --- |
| absent | Admitted bounded child/root registration | `created` | Allocate stable owner/Job ID and cumulative counters |
| `created` | Valid bindings and dispatch prerequisites registered | `queued` | Record readiness; no execution lease yet |
| `queued` | Current authorization, deadline, state, and resources valid | `running` | Grant attempt capacity; activate start/resume payload |
| `running` | Safe-point yield with valid retained state | `waiting` | Atomically create Continuation and closed wait set; release quiesced execution lease |
| `running` | Accepted supported pause reaches safe point | `paused` | Preserve state and create Continuation; release quiesced execution lease |
| `queued` | Pause accepted before dispatch | `paused` | Wrap pending start/resume payload in a Job-owned Continuation |
| `waiting` | Pause accepted | `paused` | Preserve Continuation and unresolved/ready wait condition |
| `waiting` | Wait satisfied; no pause/termination gate | `queued` | Consume Continuation into pending-resume payload exactly once |
| `paused` | Resume accepted; condition still unresolved | `waiting` | Clear pause gate; retain Continuation |
| `paused` | Resume accepted; condition satisfied | `queued` | Consume Continuation into pending-resume payload |
| `running` | Explicit retry policy; previous attempt stopped/fenced and reconciled | `queued` | New bounded attempt after not-before time; preserve Job/deadline/cumulative accounting |
| `running` | Valid success/allowed graceful-stop result, required work complete | `finalizing` | Freeze successful intent; stop useful dispatch; begin owned cleanup |
| `created`, `queued`, `waiting`, `paused`, `running` | Cancel, deadline, permanent failure, or required-child failure wins commit order | `cancelling` | Close dispatch; fix stop reason; invalidate resumability; stop/fence active work and cancel descendants |
| `cancelling` | Local execution stopped or safely fenced; outcome classification known or explicitly unknown | `finalizing` | Freeze failed/cancelled target and bounded outcome metadata |
| `finalizing` | All children terminal; own allocations released or explicitly transferred to bounded quarantine | `succeeded`, `failed`, or `cancelled` | Publish frozen terminal result; retain only bounded result/provenance metadata |

If no execution is in flight, the `cancelling -> finalizing` transition may be
immediate, but its event order remains observable. An active remote invocation is
`running` until its completion or termination processing; network waiting alone
does not falsely create a resumable Continuation. A Job has at most one active
attempt. An automatic retry is disabled unless explicitly admitted by policy.

`finalizing` is deliberately distinct from `waiting`: it cannot resume useful
work, receive new children, or acquire a new execution lease. Cleanup operations
are bounded controller obligations, not ordinary reauthorization of the workload.
Resource state may survive only as a cleanup/quarantine record. A late cancel
cannot overwrite a successful/failed intent already committed in `finalizing`.
See [Failure Model](FAILURE_MODEL.md) for uncertain cancellation classification.

## Continuation and payload invariants

| Job state | Resume-state rule |
| --- | --- |
| `created` | Bounded initial inputs only; no Continuation |
| `queued` | Initial input or one bounded pending-resume payload; no Continuation |
| `running` | Machine owns active state; no suspended Continuation |
| `waiting`, `paused` | Exactly one valid Continuation when resumable; suspension unsupported is rejected |
| `cancelling`, `finalizing` | No resumable Continuation/payload; opaque references may remain solely for accounted cleanup |
| terminal | No live Job-owned execution state; bounded references to quarantine/provenance only |

Suspension generations are monotonic within one Job. A child completion names
the owner and expected generation; after consume/discard, duplicate delivery is
ignored as stale and cannot create another wakeup. Queue waiting and resume
authorization failure never lose the obligation to release retained resources.
Moving a reference between Continuation, pending payload, and active state does
not change its allocation identity or count its bytes twice.

## Run lifecycle and activity projection

A Run is not scheduled. Its controller owns the root subtree and admission/pause
gates. During ordinary execution its displayed activity is derived in this order:

1. `running` if any owned Job has active workload execution;
2. `queued` if any Job is queued/ready to start but awaits dispatch;
3. `waiting` if any Job awaits dependencies/results, or a pause barrier is pending;
4. `paused` only when all remaining workload Jobs are paused, workload execution
   is quiescent, and the requested Run pause barrier has committed;
5. otherwise `waiting` with an explicit reason such as `jobs_paused` (all pauses
   were individually requested), `child_cleanup`, or `control_progress`.

Bounded maintenance/offload/cleanup may continue while workload execution is
paused and remains separately visible/accounted; it cannot dispatch useful work.

An individual parent being `waiting` does not make the whole Run waiting while a
child runs. These activity updates do not schedule or cancel Jobs on their own.

| From | Trigger | To / obligation |
| --- | --- | --- |
| absent | Validation, policy, and finite budgets admitted | `created`; register root |
| `created` | Root made ready | Projected `queued` |
| `queued`, `running`, `waiting`, `paused` | Subtree progress or pause/resume | Recompute activity using the rules above |
| Any nonterminal active state | Run cancellation/deadline/fatal failure wins | `cancelling`; close dispatch and stop the entire subtree |
| Active state | Root selects terminal intent | `finalizing`; close useful dispatch, preserve root intent, drain subtree |
| `cancelling` | Whole subtree stopped/fenced and terminal classification selected | `finalizing` |
| `finalizing` | Every Job terminal; all Run ownership released or transferred to bounded quarantine | Frozen terminal `succeeded`, `failed`, or `cancelled` |

A successful Run requires valid declared root outputs; failures from explicitly
collected or losing children remain provenance and do not silently become output.
Terminal status never asserts absence of remote side effects. A successful race
Run may retain explicit uncertain-loser and quarantine metadata.

## Pause and resume barrier

Targeted Job pause affects that Job, not its children. Child results may arrive
while it is paused; they satisfy its retained wait condition without resuming it.

A Run pause is a barrier over its entire current subtree. The controller first
checks that every in-flight workload operation advertises a safe pause/quiescence path;
otherwise it rejects the request as unsupported without closing the Run gate.
On acceptance it closes new-dispatch/child-creation gates and requests safe-point
pauses. Already-completing Jobs may become terminal during the barrier. Remaining
queued/waiting Jobs become paused with their start/resume state preserved.
The Run is not reported `paused` until the whole workload subtree is quiescent. Pending
pause has its own bounded request deadline; timeout rejects the pause, clears
only pause flags established by that command, and preserves pre-existing targeted
pauses. It does not reset Run/Job deadlines or discard state.

Run resume revalidates the operation and opens the gate, then clears only pauses
created by that Run barrier. Each Job follows the normal wait/queue/resume checks;
individually paused Jobs remain paused. Actual resource reacquisition can be
delayed. Cancellation or deadline during a pause/barrier has priority once its
stop intent commits; a later resume request cannot reopen dispatch.

## Machine substates and safe points

Inference has an internal progression: `initializing -> tokenizing -> prefill ->
decode -> completing`, with suspension/error exits only through the Job controller.
These are stage observations while the Job is running, not another cancel authority.
Adapters MUST advertise where they can quiesce, preserve state, stop, inject
bounded inputs, or resume. Opaque inference lacks these promises unless proven.

A decode iteration commits token position, sampler state, and backend state as a
consistent boundary before exposing a token and honoring a pause. An interrupt
received mid-iteration stays requested until that boundary. Cancellation prevents
further useful iterations once observed; the backend's bounded stop/containment
contract controls its real latency. No universal sub-token interruption is promised.

The Workflow Machine advances only compiled dependencies/bindings/control groups.
Its own coordinating Job may yield while children run and resume through the same
Continuation rules. It must not retain an execution slot while waiting. Bounded
control advancement is still subject to cancellation and deadlines.

## Required commit/event ordering

Event names here identify semantic records. The envelope and retention rules are
defined in [Event Model](EVENT_MODEL.md); one transition may append a contiguous
event group.

| Scenario | Required order |
| --- | --- |
| Start | `job.state_changed` to queued; admitted resource grant; `job.state_changed` to running; machine progress |
| Yield | Safe-point acknowledgement; atomically commit `continuation.created` then `job.state_changed` to waiting/paused; publish released execution capacity |
| Child dispatch | Parent suspension and child registration commit before child `running` or adapter dispatch |
| Child success/wake | Child terminal record; group decision; atomically `continuation.consumed` then parent queued; new resource grant; parent running |
| Pause/resume | `interrupt.requested`; safe-point/state transition group; `interrupt.applied` (or `interrupt.rejected`) |
| Cancellation | `interrupt.requested` if caller-triggered; Job cancelling/dispatch gate; `continuation.discarded` if present; descendant stop requests; stop/fence evidence; finalizing; cleanup transfer/release; terminal |
| Race winner | Accepted candidate terminal; `race.winner_selected`; loser cancellation requests; parent readiness; loser settlement before parent terminal |
| Run terminal | Each child terminal before its parent terminal; root terminal and resource transfer/release before Run terminal |

Consumers must not interpret half of a commit group as a stable snapshot. Resource
grant and release records retain their own allocation/lease identities; lifecycle
events are not proof of free memory. The order above is normative even if events
are transported later or a bounded trace contains explicit gaps.

## Deterministic arbitration cases

| Competing observations | Required decision |
| --- | --- |
| Valid success and cancel | First accepted controller commit wins; success freezes finalizing, cancel freezes stopping and blocks later success |
| Completion arrives at/after deadline | Timeout wins even if timer callback has not run |
| Child result and parent pause | Result may satisfy wait; paused parent retains it and never auto-resumes |
| Child result and parent cancel | Cancelled/closing owner does not inject result; retain only bounded outcome/provenance |
| Resume and capacity unavailable | Remain queued with pending payload and charged footprint |
| Resume and invalid state/current policy | Fail/clean up; never recreate state or reuse stale authorization silently |
| Duplicate result/cancel/resume command | Same accepted outcome or stable stale/terminal response; never duplicate child, dispatch, or injection |
| Resource release acknowledgement after terminal quarantine | Reconcile quarantine once using matching identity/epoch; do not reopen Job/Run |

These cases, including fault injection at each commit boundary, become Phase C
offline tests after Phase B specifies controllable clock/backend/adapter seams.
