# Phase B: baseline and optional acceptance-to-test design

## Status and authority

This is a **test design**, not executable tests or a report of passing tests.
Every A01–A42 ID in [Phase A acceptance](../DESIGN_ACCEPTANCE.md) is
mapped below; A41–A42 are optional strict-cost cases. Phase A owns semantics; this document chooses observable test
boundaries and controllable doubles for Phase C. A mock satisfying a contract
does not establish that a real backend or host integration supports it.

All baseline A cases except optional A41–A42 require **offline deterministic contract tests**: GPU, network, provider account and weights are not required. This
integration flag applies individually to every case below. Real-backend and
host/provider qualification is a separate gate in [Phase C plan](PHASE_C_PLAN.md).

**Scope correction:** single-user FLAMORIS native model/runtime. A41–A42 and B-PAID02 are optional strict-cost profile tests, not baseline gates. A05/A24 test finite local Run limits, A35 checks current local owner/scope. CPU reference and OpenCL are internal compute; third-party runtimes never serve as interchangeable inference backends.

## Shared fixture and oracle

Every case constructs an isolated Runtime instance or the named smaller component.
Production-factory cases construct independent native Runtime instances, including sequential and concurrent lifetimes. Component fixtures may simulate process incarnations; optional paid-budget fixtures live in a separate profile.
The fixture supplies every seam below. The case's `Active doubles` list identifies
which doubles are exercised. All others reject unexpected calls and assert a zero
call count; omission never falls back to a live service.

| Key | Planned double / controlled responsibility |
| --- | --- |
| C | `FakeClock` implementing `MonotonicClock`: explicit tick advancement, separately controllable timer delivery, unrelated wall-time jumps. No sleep. |
| X | `ManualControlExecutor`: enqueue proposals, advance one commit, delay/reorder callbacks, inspect before/after publication, and enumerate specified competing schedules. Worker operations remain pending until the fixture acknowledges them. |
| B | `FakeBackend`: scripted token/prefill/decode steps, evaluated/emitted positions and pending token, sampler/RNG/decoder/stop state, quiescence/preservation support, partial mutation/allocations, stop/release acknowledgements, malformed outputs and stale generations. Never produces a success or release merely because time advances. |
| A | `FakeCapabilityAdapter`: bounded call/outcome transcripts; independent provider acceptance, local response, cancellation, status-query and effect evidence. Has an explicit finite cost ceiling and configurable idempotency contract. |
| R | Resource test port plus real `ResourceManager` where it is the subject: integer pool sizes, unmaterialized reservations, unique allocations, references, leases, transfer overlap, uncertainty and cleanup records. Every grant/release is inspectable. |
| H | `FakeHostAuthority`: enforceable envelope grants in the simulation, epoch changes, expiry, unavailable inventory, containment evidence. A simulated grant is never evidence of deployed host support. |
| P | `FakePolicy` and immutable `CapabilitySnapshot` fixtures: principal/scope, expiration, revision, effects, pins, confirmations, explicit revocation and current availability. |
| D | Optional strict-cost fixture only; no baseline case depends on a durable paid port. |
| O | Capturing bounded event store/observer: group visibility, sequence/watermark, overflow, redaction, retention, replay and forbidden live-command sinks. |

X and O are active in all lifecycle cases. C is active even without an explicit
time race, so no test depends on real wall time. Compiler/effect-only cases assert
that no Run, Job, backend work or resource authority was created. Each fixture uses
small finite bounds and tests the relevant boundary at limit minus one, limit and
limit plus one; values below are test data, not deployment defaults.

The oracle asserts domain state, complete committed event groups, and independent
resource/effect transcripts. It does not derive all three from one implementation
getter. Important shared assertions:

- Only Job IDs enter the ready queue; no Continuation, Run or cleanup record does.
- Exactly one Job owner and at most one active attempt exist. Terminal state and
  frozen finalizing intent never change. Children become terminal before their
  parent, and root terminal precedes Run terminal.
- `a < b` below means committed semantic order, not transport delivery order.
  `{a; b}` denotes one atomic, contiguous event group. Snapshots cannot see a
  prefix; per-Run `seq` increases and references the same transition/causation.
  Resource ledger revisions have their own order, not a cross-Run event clock.
- `queue → run` expands to queued state, current checks, complete resource grant,
  then running state. `stop → terminal` expands to cancelling, resumability
  invalidation, genuine stop/containment evidence, finalizing, accounted
  release/quarantine and the frozen terminal state. Cancellation with unknown
  external outcome is failed/`outcome_unknown`; timeout is failed/timeout with
  uncertainty evidence, never successful cancellation.
- Memory accounting is `unique resident + unmaterialized reservation + uncertainty`
  within the envelope. Moving a reference or releasing an execution lease changes
  no resident-byte total. Optional strict-cost tests separately verify D liability.
- Pre-Run rejection is a bounded request error, not fabricated Run events. Every
  denied handoff has zero adapter/backend execution calls. Each scenario checks
  bounded/sanitized errors, event payloads and nested causes.

X mirrors the selected [single control executor](CONCURRENCY.md). Inject at
before-claim, prepared-receipt, before-final-dispatch, before-step-observation,
before-atomic-pause, before-terminal-transfer and post-terminal-closure boundaries.
The test cannot interleave mutation halfway through a committed control turn.
Use explicit interleavings for correctness tests. Separate barrier-controlled
multithreaded/ThreadSanitizer tests validate that the production mutex/queue
implementation preserves those same histories; timing-dependent stress is not
the acceptance oracle. Crash cases destroy instance-owned state at an injected
boundary without calling graceful-shutdown code, retain only external fixture
stores, and construct a different Runtime instance identity.

## Compiler, authority and expansion

### A01 — deterministic compilation

- **Boundary / active doubles:** validator/compiler and immutable plan; P, O.
- **Injection:** compile identical IR, registry snapshot, compiler revision and
  static limits twice; repeat with shuffled non-semantic JSON object-key order.
  Use an invalid input pair as well as a valid bounded DAG.
- **State / events:** equal normalized plan bytes/fingerprint or equal structured
  rejection; stable node/dependency order. No authorization grant, Run lifecycle
  group, scheduler entry or dispatch. A separate semantic-field mutation changes
  the fingerprint or produces rejection, rather than disappearing in normalization.
- **Resources:** zero reservations, allocations, leases and D liabilities; no
  reference resolver performs I/O.

### A02 — reject malformed or excessive plans

- **Boundary / active doubles:** bounded parser, binding validator and compiler;
  P, O.
- **Injection:** absent input/output binding, incompatible schema, cycle introduced
  only by a reference, duplicate participant/ID, non-predecessor reference, and
  graph/depth/fan-out expansion exceeding configured limits. Include a URL/path
  string in a reference position to prove it is not secretly resolved.
- **State / events:** deterministic input/plan error before admission, with logical
  field/node correlation; no partially executable plan, Job or lifecycle event.
- **Resources:** bounded parser/compiler allocation fails at the limit; zero
  backend, adapter, host, resource and paid-authority calls.

### A03 — stale contract pins

- **Boundary / active doubles:** compiler, admission and dispatch/resume guards;
  P, B, R, H, O.
- **Injection:** compile/admit, then independently replace a pinned effect,
  input/output schema, adapter revision or control contract before first dispatch
  and before resumed dispatch. An unrelated registry entry is the negative control.
- **State / events:** changed pins yield `plan_stale`; before admission no Run,
  after admission normal failed cleanup. A suspended Job cannot reach running or
  call backend resume. Unrelated changes do not invalidate the plan; no migration
  or silent new pin appears in either history.
- **Resources:** return unmaterialized grants; preserved allocations remain charged
  until release acknowledgement/cleanup transfer. No new paid handoff.

### A04 — current authorization at handoff

- **Boundary / active doubles:** admission/dispatch guard and Job controller;
  P, A, R, H, O.
- **Injection:** revoke capability, concrete object scope, confirmation or context
  validity after compile/admission and immediately before handoff. Repeat on retry
  and resume, and for pure/read as well as effectful work.
- **State / events:** `authorization.denied` precedes failed stopping/finalization;
  no `attempt.dispatch_committed` for the denied attempt and no useful adapter call.
  A revocation observed after an already committed handoff requests supported stop
  and preserves any confirmed/unknown effect instead of rewriting history.
- **Resources:** rollback unused grants; cleanup/approved reconciliation still
  runs under bounded internal authority. Revocation cannot strand accounting.

### A05 — shared finite Run allowance competition

- **Boundary / doubles:** Run ledger and dispatch eligibility; A, R, O.
- **Injection:** two child attempts each require six slots/units from a Run
  allowance of ten. Enumerate both orderings and cancellation versus dispatch.
- **State / events:** only the fitting set commits; rejection is bounded and
  does not spend a different Job's allowance. No tenant or durable money ledger.
- **Resources:** local cumulative reservations and real allocations agree;
  unknown external effects remain in provenance, not an invented refund.

### A06 — effect algebra

- **Boundary / active doubles:** `EffectSet` validation/aggregation; P, O.
- **Injection:** empty, unknown, pure-plus-any-other, destructive-without-write;
  valid pure, read+write, external+paid and write+destructive. Compose an all-pure
  collection and pure plus external/paid; include invalid members in compositions.
- **State / events:** exact deterministic rejection of invalid metadata; valid
  summaries are `{pure}` and `{external, paid}` respectively. Invalid members are
  not silently repaired; an empty graph cannot acquire purity. No Run/events.
- **Resources:** no reservations, execution calls, ambient read or paid liability.

### A07 — untrusted dynamic proposal envelope

- **Boundary / active doubles:** inference proposal parser, fragment compiler,
  current admission and parent controller; B, A, P, R, H, D, O.
- **Injection:** model proposes raw endpoint/credential, absent capability,
  out-of-scope target, recursive ownership/depth, excessive fan-out and repeated
  invalid requests. Set proposal budget 3 and parent deadline 100 ticks; also test
  a valid child exactly within the envelope.
- **State / events:** invalid proposals create no dispatchable child. At most the
  admitted bounded error-recovery path runs; the fourth proposal cannot expand the
  budget. Valid fragment fingerprint/child registration and parent suspension
  commit before child dispatch; no new capability/permission is installed.
- **Resources:** failed proposals consume the declared proposal count but no
  execution/cost reservation; valid children share original cumulative limits and
  cannot extend the parent deadline or escape into detached work.

## State, resources and control races

### A08 — same-Job yield and capacity wait

- **Boundary / active doubles:** InferenceMachine, controller and Scheduler;
  B, A, R, H, P, O.
- **Injection:** acknowledged safe yield, successful child, then occupy the sole
  execution slot until after the parent becomes ready. Include a sampled/emitted
  token not yet evaluated by the backend and partial UTF-8/stop-matcher carry.
- **State / events:** same parent ID follows running → waiting → queued → running.
  Safe-point evidence precedes `{continuation.created; waiting}` and lease release;
  child terminal precedes `{continuation.consumed; queued}`. Exactly one pending
  resume payload survives the queue wait and is consumed only after a new grant.
- **Resources:** no lease in waiting/queued state; the same state allocation stays
  charged once. No second backend initialization, token sampling/ingestion or
  sampler/RNG/decoder reset occurs; pending token is evaluated exactly once.

### A09 — duplicate and late wakeups

- **Boundary / active doubles:** callback ingress, controller and resume injection;
  B, A, R, H, P, O.
- **Injection:** deliver child completion twice, then after resume and after parent
  cancellation. Independently mismatch owner, suspension generation, attempt and
  runtime incarnation; replay an old completion during a later suspension.
- **State / events:** one valid `continuation.consumed` and at most one result
  injection/dispatch. Stale delivery cannot recreate a Continuation, overwrite a
  binding or change cancelling/finalizing/terminal state. Its bounded diagnostic
  is coalesced and never another lifecycle transition.
- **Resources:** duplicate callbacks grant/release nothing; legitimate late cleanup
  evidence goes only to the matching cleanup owner, never the active replacement.

### A10 — retained-state dependency deadlock

- **Boundary / active doubles:** ResourceManager feasibility and parent/child
  coordination; B, R, H, P, O.
- **Injection:** device capacity 10, retained parent state 7 and required child
  working set 5. Branches: approved offload with enough RAM/staging; offload
  unsupported; insufficient target headroom; acknowledgement lost.
- **State / events:** successful offload can make the child eligible only after
  real source-release evidence. Otherwise bounded resolution fails
  `resource_unavailable` with `resource_deadlock` and settles the owned subtree;
  no endless queue, forced state eviction or re-inference-as-resume.
- **Resources:** lease release leaves 7 resident; successful transfer temporarily
  charges source and target/staging. No child consumes fictional free memory.

### A11 — shared model allocation and quotas

- **Boundary / active doubles:** allocation/reference ledger and Run quotas;
  B, R, H, P, O.
- **Injection:** two Runs refer to one 6-unit model and own 2-unit private states
  each; release one Run, then request eviction while the other reference is live.
  Try a third Run whose individual working-set quota is less than 8.
- **State / events:** first release cannot report model eviction; only last
  dependency release plus backend acknowledgement permits model release. The
  under-quota Run is rejected even though the physical model is shared.
- **Resources:** physical total 10 initially, then 8 after first private-state
  release, not 16 or 2. Each admitted Run's working-set check includes 6+2.

### A12 — offload copy without source release

- **Boundary / active doubles:** transfer protocol and allocation ledger;
  B, R, H, P, O.
- **Injection:** validate target copy, suppress source release acknowledgement and
  expire the bounded transfer/cleanup deadline. Later acknowledge only the old
  source. Repeat with a corrupt target copy.
- **State / events:** copy-complete is not evacuation-complete; emit uncertainty
  rather than release-confirmed. No replacement device admission uses the source
  bytes. Corrupt copy is not valid resume state; safe source is retained if possible.
- **Resources:** source and valid/uncertain target plus staging stay charged until
  their individual acknowledgements. Late matching source release removes that
  one charge; no duplicate release or unaccounted partial target.

### A13 — old epoch release callback

- **Boundary / active doubles:** host/backend fencing and reconciliation ledger;
  B, R, H, P, O.
- **Injection:** allocate under epoch 1, change host/backend epoch, create a distinct
  epoch-2 record only after a new valid envelope, then deliver epoch-1 release with
  a reused opaque backend handle. Include an unknown current envelope branch.
- **State / events:** epoch mismatch cannot mutate epoch-2 state. Old cleanup may
  reconcile only its own identity. Affected dispatch is fenced while authority is
  unknown/unreconciled; unrelated verified resources continue independently.
- **Resources:** epoch-2 bytes/lease remain untouched. Old liability stays charged
  until authoritative old-record reconciliation; stale free-memory samples grant
  neither host ownership nor replacement capacity.

### A14 — cancellation versus valid completion

- **Boundary / active doubles:** Run commit arbitration and Job finalization;
  B, A, R, H, P, O.
- **Injection:** enumerate cancel-before-success and success-before-cancel, both
  before deadline, plus duplicated commands/callbacks. Completion includes valid
  bounded output and satisfied required work. Model an already-mutated native step
  whose safe-point receipt has not yet been accepted by the controller.
- **State / events:** cancel first fixes stopping and blocks result injection;
  verified stop follows cancelling → finalizing → cancelled. Success first fixes
  finalizing(success) → succeeded and later cancel cannot change intent. Inspect
  each complete event group, not callback arrival order. If stop wins before that
  step receipt, suppress its token publication and mark advanced native state
  cleanup-only; never pretend an aborted/partially processed step rolled back.
- **Resources:** useful dispatch closes at the winning commit; terminal waits for
  actual release/containment. A late successful external effect remains evidence
  even when local cancellation won.

### A15 — deadline before timer delivery

- **Boundary / active doubles:** completion/dispatch deadline guards; B, A, R, H,
  P, O, with independent clock/timer delivery.
- **Injection:** deadline 100; hold timer callback and offer completion at 99,
  100 and 101. Repeat after queue wait, pause, child wait and retry backoff; jump
  wall time without changing monotonic time.
- **State / events:** 99 may freeze success; 100/101 select failed timeout even
  without timer delivery. Expired queued/resuming/retrying work never runs; original
  deadline remains 100. Stop/finalizing/terminal order follows the shared oracle.
- **Resources:** cleanup gets its separately bounded allowance only; no useful
  work is authorized by that allowance. Memory/liability remain until evidence.

### A16 — targeted parent pause with active child

- **Boundary / active doubles:** targeted pause, wait condition and Run projection;
  B, A, R, H, P, O.
- **Injection:** parent waiting for child receives targeted pause; child then
  succeeds. Reverse child-completion/pause commit order as a second schedule.
- **State / events:** parent paused with satisfied wait, no automatic resume or
  result injection. Child running still projects Run running; after it settles,
  all individually paused work projects waiting/`jobs_paused`, not Run paused.
  Authorized targeted resume consumes the preserved wait result exactly once.
- **Resources:** parent has no execution lease, retains state bytes; child owns
  its own lease until quiescence/release. No Run-wide pause gate is fabricated.

### A17 — unsupported Run pause and pending barrier

- **Boundary / active doubles:** atomic pause preflight/commit; B, A, R, H, P, O.
- **Injection:** subtree includes one opaque running operation without safe pause
  and one pausable Job. Then test a fully supported subtree with delayed safe-point
  acknowledgements.
- **State / events:** unsupported command returns `interrupt.rejected`, no pause
  flags/gates or partial successful pause. Supported acceptance records one barrier
  generation/target set; remains pending until every captured workload quiesces,
  then `interrupt.applied` and Run paused. No new useful dispatch while pending.
- **Resources:** delayed acknowledgement keeps real execution capacity accounted;
  paused retained memory is not released. Bounded maintenance remains distinct.

### A18 — pause barrier timeout and rollback

- **Boundary / active doubles:** pause-cause ownership and control deadline;
  B, A, R, H, P, O.
- **Injection:** Job T is already targeted-paused; Run barrier pauses Job U while
  V withholds safe-point acknowledgement. Expire only the command deadline;
  revoke U's resume permission before rollback.
- **State / events:** reject the timed-out barrier, clear only its flags/gate,
  retain T's pause and every valid wait/state reference. U must pass normal resume
  checks and cannot run under stale authority; V never receives a false paused
  assertion. Late barrier acknowledgement cannot reapply the expired command.
- **Resources:** no state is silently discarded/refunded; no new lease until
  reauthorized complete admission. Original workload deadlines do not reset.

### A19 — unusable or unauthorized resume

- **Boundary / active doubles:** PendingResume validation and cleanup;
  B, R, H, P, O.
- **Injection:** paused or queued-resume parent has incompatible/lost backend
  state, changed epoch, or revoked/expired permission. Test each independently.
- **State / events:** `state_unavailable` or `permission_denied` as applicable,
  cancelling → finalizing → failed; discard resumability and preserve cause.
  No backend initialization, fresh prefill, equivalent-resume claim or result
  injection; no `queued → running` for the rejected resume.
- **Resources:** retained references transfer to accounted cleanup until release
  or proven containment; zero replacement execution lease.

## Coordination and external outcomes

### A20 — required child timeout

- **Boundary / active doubles:** all-success coordination and subtree finalizer;
  B, A, R, H, P, O.
- **Injection:** parent awaits required MCP child; that child times out while a
  sibling runs and no recovery branch is declared. Delay sibling stop evidence.
- **State / events:** timeout closes child dispatch, stops it, then child failed
  precedes required-join failure and parent cancellation. Parent Continuation is
  discarded, unfinished sibling stopped, all children terminal before parent and
  Run terminal. Unknown child outcome remains attached to `job_timeout`.
- **Resources:** no success injection or early parent terminal; retained parent
  state and sibling capacity release/transfer only on evidence. This test executes
  every ordered step of Phase A's worked MCP-child failure trace.

### A21 — all-settled typed collection

- **Boundary / active doubles:** WorkflowMachine all-settled and bindings;
  A, R, H, P, O.
- **Injection:** three declared children finish failure/success/cancelled in an
  order different from declaration; one completion is delayed. Separate branch
  expires the group deadline before the last terminal result.
- **State / events:** no early `join.completed`; all terminal records precede the
  bounded typed collection in declared order and parent wake. Failures remain
  typed data, never success payloads. Deadline branch fails timeout and stops
  unfinished children without inventing a settled result.
- **Resources:** each child settles independently; bounded output/event capacity
  reserved for all outcomes, no implicit retry/cost refund or hidden call.

### A22 — race selection and unacceptable candidates

- **Boundary / active doubles:** race acceptance and serialized controller;
  A, R, H, P, O.
- **Injection:** two valid successes delivered in each commit order, then in one
  collected batch; first success fails the bounded acceptance predicate; all
  candidates fail or are unacceptable; deadline expires before acceptance.
- **State / events:** earliest accepted success wins, batch tie uses declaration
  order. One candidate terminal < one `race.winner_selected` < loser stop requests
  < parent readiness. Failure/rejected output never wins; exhausted candidates
  produce `race_no_acceptable_result`, deadline produces timeout.
- **Resources:** acceptance invokes no I/O or capability; only the winner output
  binds, losing state remains bounded and owned until settlement.

### A23 — immutable winner and uncertain remote loser

- **Boundary / active doubles:** race, external cancellation and cleanup ownership;
  A, R, H, P, D, O.
- **Injection:** choose winner, refuse loser stop confirmation, then provide proven
  local containment while remote outcome/cost remains unknown. Deliver loser
  success after parent terminal as a separate branch.
- **State / events:** winner never changes; loser fails `outcome_unknown` after
  safe containment. Parent may resume earlier but cannot terminalize before loser
  terminal. Run success may carry explicit loser uncertainty/quarantine. Late
  success is reconciliation evidence, not winner replacement or rollback.
- **Resources:** loser remote concurrency/debt and full unresolved D liability stay
  charged; terminal follows bounded quarantine transfer, never imaginary release.

### A24 — forbidden or over-limit races

- **Boundary / doubles:** compiler, Run ledger, dispatch guard; P, R, O.
- **Injection:** forbidden write/destructive race; an allowed group whose
  aggregate participants/attempt slots exceed the finite Run envelope.
- **State / events:** reject before any participant dispatch with a typed
  plan/policy/limit error. Include losing and retry attempts in the bound.
- **Resources:** no partially useful dispatch or unaccounted reservation.
  Strict monetary race funding is an optional profile, not this case's gate.

### A25 — accepted write with lost response

- **Boundary / active doubles:** registered adapter outcome/retry/reconciliation;
  A, R, H, P, D, O.
- **Injection:** adapter records provider acceptance of a write, loses its response,
  and returns unknown to a bounded registered status query. Repeat with policy
  permitting generic transient retries but no sufficient provider dedup guarantee.
- **State / events:** one `attempt.dispatch_committed`; then unknown outcome and
  normal failed/`outcome_unknown` cleanup. No blind retry, success binding or
  cancelled claim. Reconciliation is a separate authorized, bounded status call
  and never repeats the original write.
- **Resources:** finite query budget consumed; uncertain operation/concurrency and
  any paid maximum remain accounted. No refund is inferred from response loss.

### A26 — write succeeds but result is rejected

- **Boundary / active doubles:** streaming result validation and binding/injection;
  A, B, R, H, P, D, O.
- **Injection:** confirmed write returns wrong schema, invalid handle/encoding or
  output crossing the size limit while receiving. Supply a sensitive raw error
  payload to verify nested sanitization.
- **State / events:** `invalid_result` or `result_too_large`, confirmed-success
  effect evidence, failed child and declared propagation/recovery. No rejected
  bytes enter downstream bindings or inference; no repeat write to recover output.
- **Resources:** receive buffer never exceeds its bound; provider asset remains
  provider-owned; paid settlement uses authoritative cost, not output validity.

## Submission, containment and observation

### A27 — atomic concurrent submission claim

- **Boundary / active doubles:** submission authority, canonical digest and pending
  waiter completion; P, R, H, D, O.
- **Injection:** pause owner after scoped-key/Run-ID claim, submit same-digest waiter
  and different-digest contender. Branch into successful admission and pre-Run
  rejection; insert a new submit before and after waiter rejection publication.
  Repeat after retention expiry and after instance reconstruction.
- **State / events:** one pending owner/Run identity; same digest shares admitted
  Run or identical bounded rejection; different digest conflicts while pending.
  Rejected claim releases only after current waiters are notified; terminal failure
  retains an admitted claim through its window. No duplicate root or dispatch.
- **Resources:** non-owners reserve nothing. Rejected owner rolls back unused
  bookkeeping safely. After expiry/restart a new admission is possible, never a
  cross-restart exactly-once promise or proof that the old operation did not run.

Unkeyed submission variants (same A27 boundary/doubles): submit identical content
without a key twice, both sequentially and with both pending before admission.
Assert distinct PendingSubmissionIds and distinct Run/root identities on success,
zero SubmissionIndex entries/lookups or attached duplicate waiters, and independent
resource/budget checks. Reject one owner while the other succeeds; late receipt or
transport disconnect must not attach to or cancel the other. Repeat with an explicit
key to recover the original one-Run claim behavior. Present null/empty/non-string/
257-byte keys reject without Run events or resource acquisition; a 256-byte key
is accepted subject to ordinary checks. Digest equality alone never deduplicates.

### A28 — inability to stop or contain

- **Boundary / active doubles:** Job finalizer, backend lifetime and host containment;
  B, A, R, H, P, O.
- **Injection:** native worker keeps writing after stop request and cleanup deadline;
  separately, isolated worker supplies valid containment evidence. A callback-fence
  acknowledgement without physical containment is deliberately insufficient.
- **State / events:** wedged in-process Job stays unresolved/cancelling with
  `cleanup_timeout`; no invented quiescence, finalization completion or terminal.
  Proven containment permits accounted transfer and subsequent terminal. Surface
  supervisor-required status without issuing arbitrary host service commands.
- **Resources:** affected capacity is unavailable; no replacement overlaps the old
  writer and no worker-owned context is destroyed while it may still be accessed.

### A29 — bounded post-terminal reconciliation

- **Boundary / active doubles:** cleanup ledger plus retained Run event projection;
  B, R, H, P, D, O.
- **Injection:** transfer one quarantined allocation before terminal, acknowledge
  its matching ID/epoch during retention, repeat/stale the ack, then deliver a
  distinct valid late ack after expiry. Exhaust closure capacity before transfer
  as a negative branch.
- **State / events:** closure/update capacity is reserved before transfer; absent
  capacity blocks that terminalization route. Terminal group < increasing-seq
  `reconciliation.observed` < `reconciliation.closed`; terminal intent is immutable.
  Repeated/stale ack creates no duplicate transition or unbounded events. Expired
  Run returns explicit expiration/gap and never receives newly invented events.
- **Resources:** matching authoritative release updates the ledger exactly once,
  even after Run retention expires; event overflow cannot block cleanup accounting.

### A30 — bounded event storage and slow subscribers

- **Boundary / active doubles:** admission event reservation, EventStore and observer;
  B, A, R, H, P, O.
- **Injection:** fill optional token/debug buffer, never drain one subscriber, then
  exhaust normal control admission capacity and issue cancel/cleanup. Repeat
  duplicate no-op commands/callbacks beyond their coalescing limit.
- **State / events:** stop additional admission lacking required records; existing
  reserved cancellation/terminal groups still commit atomically. Telemetry loss is
  explicit in bounded gap/drop summary; no control group is silently dropped or
  subscriber-dependent. No-op commands cannot consume unbounded reserve.
- **Resources:** total event/subscriber bytes stay bounded; worker stop and resource
  release proceed despite backpressure. Post-terminal closure capacity is distinct.

### A31 — cursor gap and incomplete group

- **Boundary / active doubles:** retained event read/projection; P, O.
- **Injection:** read from an aged-out sequence, overflow a subscriber, and import
  an inspection fixture ending midway through a persisted event group. Also read
  from exactly the earliest retained group and current snapshot watermark.
- **State / events:** explicit gap with earliest available sequence and snapshot
  watermark; partial group excluded from authoritative projection and marked
  incomplete. Complete retained suffix applies once, in order. No fabricated
  uninterrupted history or inferred external outcome.
- **Resources:** bounded observation buffers only; zero live Job, backend, adapter,
  Scheduler, allocation/cleanup and D operations. No durable journal is required
  to test handling of an incomplete inspection input.

### A32 — observation-only in-memory replay

- **Boundary / active doubles:** isolated ReplayProjection and retained group
  reader; P, O, with all execution sinks forbidden.
- **Injection:** replay a captured bounded trace containing paid/write handoffs,
  cancellation, allocation/quarantine and post-terminal reconciliation. Include
  gaps, redacted records and unsupported record versions.
- **State / events:** only a separate playback projection advances under original
  IDs/order; explicit incomplete status for unavailable content. Live Run/snapshot,
  registry/policy and event sequence remain identical; replay cannot publish into
  live control ingress or masquerade as resumed execution.
- **Resources:** backend, adapter, Scheduler, resource acquisition, release/cleanup
  and paid-authority mutation counts all stay exactly zero. This test is a baseline
  requirement, not postponed until durable journal/recovery work.

### A33 — process failure around handoff

- **Boundary / active doubles:** instance lifetime, external operation transcript
  and fresh-start admission; A, R, H, P, D, O.
- **Injection:** crash just before committed handoff, after intent before adapter
  receipt, after provider receipt and before outcome publication. Retain external
  fixture state; discard all Runtime objects without synthesized shutdown events.
- **State / events:** new runtime incarnation has no resumed Run/Job/Continuation;
  old trace may end at any committed group and cannot certify non-execution.
  Old IDs/callbacks cannot create live work. No fabricated old terminal result.
- **Resources:** affected host envelope starts unavailable pending authoritative
  reconciliation; unresolved paid reservations persist in D. No lease reconstruction
  or automatic resubmit from replay. Confirmed pre-handoff proof is distinguished
  from an ambiguous intent/receipt gap.

## Fairness, access and revised architecture obligations

### A34 — bounded fairness under warm work

- **Boundary / active doubles:** Scheduler ordering and resource admission;
  B, R, H, P, O.
- **Injection:** older eligible cold-model Job competes with continuously proposed
  warm Jobs. Exercise the reviewed four-class policy, one-second aging, protection
  by three seconds and five-second acquisition bound clipped to original deadlines;
  keep dependencies,
  explicit write ordering and separate priority classes in the fixture. Test both
  reclaimable capacity and impossible retained-state capacity.
- **State / events:** older work cannot be bypassed indefinitely; at the bound
  conflicting warm admission stops and bounded reclaim/admission proceeds, or
  explicit resource/deadline failure occurs. A blocked dependency never becomes
  eligible because its model is warm; stable queue tie-breaks are repeatable.
- **Resources:** no evict of live required state or temporary overcommit; eviction
  requires acknowledgement. No claim that fairness guarantees an impossible deadline.

### A35 — current observation and handle access

- **Boundary / doubles:** status/result/media/trace/export/replay facade; P,A,O.
- **Injection:** current local user with insufficient trace or object scope,
  revoked owner permission and expired handle; authorized redacted access control.
- **State / events:** independent current checks for each surface, no sensitive
  nested cause or expired handle materialization. Denial does not change Run.
- **Resources:** denied access performs no hidden media fetch or execution.
  Multi-tenant/cross-user isolation is outside baseline.

### A36 — conflicting unordered effects

- **Boundary / active doubles:** compiler effect/order analysis; P, O.
- **Injection:** sibling writes to the same/unknown target and unordered read/write
  conflict; then add explicit dependency or trusted contract proving disjoint
  concrete scopes. A workflow-authored disjointness claim alone is insufficient.
- **State / events:** reject ambiguity before Run admission. Accept explicit order
  or trusted disjoint proof, retain its contract pin in the plan. Configuring one
  worker/one device does not turn physical serialization into semantic order.
- **Resources:** zero dispatch and reservations for rejected input; compiler does
  not call the adapter to discover targets or manufacture effect guarantees.

### A37 — all Jobs targeted-paused

- **Boundary / active doubles:** Run activity projection and pause-cause registry;
  B, R, H, P, O.
- **Injection:** individually pause every remaining workload Job without a Run
  barrier; issue Run.resume, then the corresponding targeted resumes. Also mix an
  older targeted pause with a completed Run barrier.
- **State / events:** report waiting with `jobs_paused`; no Run-wide paused claim.
  Run.resume clears only its own barrier flags and cannot undo targeted pauses.
  Targeted resumes follow normal wait-satisfied/queue/current-guard transitions.
- **Resources:** quiesced leases absent, retained state still charged; no dispatch
  created solely by a projection update or unrelated resume command.

### A38 — control after successful finalizing

- **Boundary / active doubles:** frozen terminal intent and bounded finalization;
  B, R, H, P, O.
- **Injection:** accept success at tick 99 with workload deadline 100, withhold
  release acknowledgement, advance past 100 and deliver cancel. Then complete
  cleanup or produce safely contained residue before its separate allowance ends.
- **State / events:** finalizing(success) remains fixed; neither timeout nor cancel
  selects a different terminal result or resumes work. Required cleanup/transfer
  precedes succeeded; cleanup uncertainty is separately visible. Unsafe containment
  still blocks completion as in A28.
- **Resources:** no new useful lease; actual/uncertain allocation remains charged
  through cleanup and quarantine, independent of the successful result intent.

### A39 — direct inference follows compiler/plan path

- **Boundary / active doubles:** submission normalizer, shared compiler/admission,
  inference dispatch; B, P, R, H, O.
- **Injection:** equivalent direct request and explicit single-root inference
  workflow under identical snapshot/limits; then stale pin and permission changes.
  Test empty versus permitted bounded child envelope and unbounded direct input.
- **State / events:** both produce an immutable normalized plan with the same
  semantic fingerprint when canonical semantic fields match, model/backend/schema/
  effect pins and finite limits. Both create root through the same guards; neither
  path dispatches after stale pin/current denial. Unbounded direct request rejects.
- **Resources:** equivalent semantic requirements produce equivalent accounting;
  direct submission has no hidden load/lease before compilation/admission.

### A40 — child creation versus atomic Run pause

- **Boundary / active doubles:** RunController child registration/pause barrier;
  B, A, R, H, P, O.
- **Injection:** stop immediately before and after the single pause preflight/gate/
  snapshot commit; offer pausable and non-pausable child registration/dispatch on
  both sides. Include a precommitted eligible child whose adapter has not started.
- **State / events:** earlier committed child is included in the captured subtree
  or causes whole-command rejection. Later proposal cannot create/dispatch a child
  behind the accepted gate. There is no observable commit where preflight succeeds
  but an uncounted child enters before gate closure. One barrier generation only.
- **Resources:** rejected later child acquires no lease/budget; existing admitted
  state remains accounted. Captured in-flight work must genuinely quiesce.

### A41 — optional strict-cost crash accounting

This ID is retained for the separately reviewed Paid Budget Authority profile.
The baseline makes no cross-crash monetary ceiling promise. When that profile is
implemented, use the original durable reserve → unknown outcome → restart →
conservative liability/settlement interleavings in the
[optional contract](PAID_BUDGET_CONTRACT.md). Exclude from baseline completion.

### A42 — optional strict-cost authority unavailability

This ID is retained for the same optional profile: missing/incomplete authority
must prevent a claimed strict monetary guarantee. No baseline paid-budget fake
or durable inventory is required. Ordinary external capability authorization,
finite local limits and retry/uncertainty rules remain baseline.

## Supplemental Phase B test obligations

The A numbers remain unchanged. Phase B adds tests for implementation mechanisms,
not replacements for Phase A behavior:

| ID | Deterministic obligation |
| --- | --- |
| B-LIFE01 | Enumerate the complete Job transition table plus every disallowed pair; invalid transitions produce no partial mutation/event group. Verify Continuation presence only in eligible suspended states and one owner at each transfer. |
| B-CALL01 | Close callback ingress during shutdown; deliver stale and duplicate completions before/after controller destruction. No pointer dereference, use-after-free, unbounded queue growth or lost resource cleanup identity. |
| B-EVENT01 | Inject allocation/serialization failure at each fallible preparation stage; no state mutation or partial group follows. Inspect every commit visibility boundary: publication is no-fail and exposes the whole group/state only. An injected invariant breach inside commit requires fail-stop containment with no successful-commit claim, not fallible rollback. Reserved control capacity remains usable for bounded shutdown. |
| B-RETRY01 | Explicit retry only after previous attempt stopped/reconciled, original deadline and Job ID retained, new bounded attempt ID, stable provider operation key, cumulative cost/current authorization rechecked; no retry from cancelling/finalizing/terminal. |
| B-INPUT01 | Graceful stop only for an advertised partial-output contract; bounded injection only at declared slots, with schema/current access and command dedup. Evaluate an already committed pending token first, append approved input tokens and penalty history once without RNG draw, preserve output-only stop/decoder state, and sample only after final injected prefill. Test with/without pending token; unqualified grammar/injection profiles reject. |
| B-SER01 | Duplicate JSON keys, unsupported versions, oversized nesting/counts/strings and invalid encoding/non-finite numbers reject deterministically before side effects; canonicalization preserves distinct semantic fields and normalizes only specified non-semantic variation; sensitive nested causes are redacted before storage. |
| B-DRAIN01 | Graceful shutdown closes new admission, fixes drain and cleanup deadlines, cancels remaining work, joins genuinely stopped workers before releasing their owners, and exposes unresolved containment without pretending destructors prove deallocation. |

### B-PAID02 — optional strict-cost delivery proof

The one-shot delivery gate, durable no-send proof and crash accounting are
specified only for the optional Paid Budget Authority profile. They are not
baseline acceptance or a Phase C prerequisite.

### B-NATIVE01 — native holder lifetime and instance independence

- **Boundary / doubles:** two Runtime roots, instrumented model/session holders,
  allocation ledger and callback barriers; C/X/B/R/O.
- **Injection:** construct and destroy instances sequentially and concurrently;
  fail one partial model load; block a callback while its owning instance drains.
  Admit two Runs sharing one model, terminate the first, then release the second.
- **State / events:** no instance can release another's native state. No universal
  process guard rejects a second instance. A Job terminal does not imply model
  allocation release. A genuine process-scoped OpenCL context, if introduced,
  outlives all users and settles once after last release.
- **Resources:** unique physical charge until matching quiescence/release;
  uncertain partial allocation remains charged or safely contained. Never infer
  release from queue emptiness or a destructor alone.



Activation/startup cases B-ACT01–B-ACT12 are defined by the
[activation contract](ACTIVATION_CONTRACT.md) and
must be implemented with the same C/X/R/H/P/D fixture, not a live service manager.
Selected real-backend conformance cases are listed separately in the Phase C plan;
an unsupported backend feature is tested as explicit rejection rather than skipped
architecture semantics.
