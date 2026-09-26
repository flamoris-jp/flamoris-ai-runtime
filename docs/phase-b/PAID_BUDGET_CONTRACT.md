# Durable Paid Budget Contract

**Phase B integration design; no budget service or adapter is implemented.**
[Authorization Model](../AUTHORIZATION_MODEL.md) owns hard-budget semantics.
This document specifies the required external authority contract without selecting a
database, service, or transport. A process-local ledger and retained event trace cannot
satisfy this contract. A production paid adapter remains disabled until both durable
authority conformance and provider-enforced maximum liability are demonstrated.

## Authority and identities

The external Paid Budget Authority serializes all charged and reserved liability for
one tenant/budget scope, including other Runtime instances using that scope. Runtime
retains a cumulative Run ledger, but cannot increase a durable balance, refund unknown
cost, or recover Jobs from budget records. `PaidBudgetAdapter` owns bounded request/
response translation; only the control executor commits the returned evidence to a
live Run or cleanup observation. The adapter cannot schedule provider work.

| Proposed value | Binding and lifetime |
| --- | --- |
| `TenantBudgetId` | Trusted tenant plus budget/unit/period identity; caller input cannot choose another tenant's ledger |
| `OperationId` | Stable logical operation and provider idempotency scope; immutable principal, capability, request digest and provider-contract revision |
| `AttemptId` | Unique attempt under the same Job and operation; a retry uses its own reservation and never resets cumulative limits |
| `DurableReservationId` | Authority-issued durable identity bound to scope and concrete operation/attempt or unbound FundingSlotId, maximum liability and current record revision |
| `PaidReservationReceipt` | Bounded reference to the durable record; current reserved/armed/settled state and authority revision; not a bearer permission |
| `DeliveryGateId` | Fresh incarnation-scoped one-shot identity allocated before arm, permanently bound to concrete tenant/operation/attempt/reservation/digest; never recycled or used for another retry |
| `HandoffTicket` | Authority-issued marker bound to that attempt and DeliveryGateId before local dispatch; armed liability survives lost response/crash |
| `BudgetScopeCertificate` | Current inventory/reconciliation status and conservative remaining balance for the scope; a stale balance snapshot cannot authorize spending |
| `RaceFundingPreparation` | Run-owned bounded bookkeeping for one closed race group: all participant/operation/attempt slots, maxima, receipts and funding gate; no independent scheduling identity |
| `FundingSlotId` | Stable durable group/participant/operation-slot/attempt-ordinal identity bound to a finite envelope and maximum; an unbound slot is not a live Job or Attempt |

Runtime instance, Run and owning Job IDs are provenance fields on every reservation;
an unbound race slot names its coordinating Job, then records the concrete target
Job/operation/attempt at binding. It never fabricates a future child Job. A new
process creates new instance/Run/Job IDs; unresolved old operation identities remain
visible in durable inventory. Operation IDs and attempt keys must not be reused after
restart. The authority persists their immutable binding before acknowledging reserve.
Provider idempotency identity is stable across permitted attempts of one operation and
includes its digest/scope; an attempt ID is not automatically a provider deduplication key.

Cost uses checked nonnegative integers in an explicitly registered billing unit/currency
and scale. Mixing units, overflow or unknown conversion fails before reserve. Monetary,
billed-token and quota bounds may form a finite vector. Conversion uses conservative
bounds; floating-point estimates cannot establish a hard ceiling. The provider contract
must enforce every promised upper bound, including streaming/continuation charges and
any cancellation overrun. If that cannot be proved, hard-budget dispatch is rejected.

## Proposed asynchronous port

Every operation carries a stable request identity, expected immutable digest/binding,
bounded deadline, and the known record revision where applicable. Same identity and
same payload is idempotent; changed payload conflicts. Timeout/lost response is unknown,
not a negative acknowledgement. Authority replies are authenticated/configured evidence,
not model-supplied data. All calls run outside the Runtime control executor.

| `PaidBudgetAdapter` operation | Authority obligation |
| --- | --- |
| `inspect_scope` / `inventory` | Certify current remaining balance, unresolved record coverage and reconciliation status; bounded pages with a complete snapshot cursor/revision, never infer completeness from an empty partial page |
| `reserve` | Atomically compare aggregate charged + held liability with the limit, create one durable concrete-attempt reservation before acknowledgement, or reject without a new charge |
| `reserve_envelope` | Same atomic budget check for one identified race funding slot under a pinned finite envelope; no live attempt or permission to arm |
| `bind_attempt` | Once only, bind a funded slot to concrete OperationId/AttemptId/capability/input digest within its immutable envelope and maximum; no release/re-reserve gap, new liability or widening |
| `arm_handoff` | Atomically bind DeliveryGateId and move that reservation to durable handoff-possible state; repeat the same binding returns its ticket/current status; a closed tombstone or changed gate binding cannot arm |
| `cancel_before_handoff` | Release only with matching proof that the delivery gate closed before any handoff and cannot reopen; compete atomically with arm/settle |
| `settle` | Apply confirmed bounded cost/effect evidence once; retain unresolved cost components; never treat missing evidence as zero |
| `reconcile` | Read/status-only provider evidence or conservative maximum-liability accounting; certify safe remaining balance independently of Runtime lifecycle |

`reserve` itself enforces current durable limits; `inspect_scope` is not a pre-authorized
balance snapshot. An incomplete/unavailable inventory blocks new hard-budget paid
admission in its affected scope. A prior valid certificate cannot bridge an authority
outage at dispatch. Unaffected non-paid work follows its own policy/resources.

## Paid race funding barrier

Before **any** participant of a paid race becomes dispatch-ready, the controller
creates `RaceFundingPreparation` for its closed participant set. Checked arithmetic
derives the full maximum over every participant, allowed paid operation and permitted
attempt, including losers and retry slots. Reserve this sum atomically in the Run
ledger, together with its child/attempt bookkeeping allowance. This is distinct from
acquiring every participant's physical execution vector simultaneously; Scheduler may
still serialize the race when resources require it.

Preparation is an admission prerequisite for the existing coordinating Job's first
`queued -> running` transition. While asynchronous budget replies are pending, that
Job stays queued with no execution lease or Continuation. Its deadline still runs.
This is bounded resource/paid-admission bookkeeping, not a synthetic network-waiting
running Job, activation worker, or additional scheduler identity.

The maximum includes nested paid races transitively. A nested race consumes a disjoint
subset of its enclosing preparation's `FundingSlotId` records; it neither reserves
nor charges those amounts again. Its own funding gate validates every
required subset receipt before participant dispatch. Each slot has exactly one funding
owner and one consumer path; siblings cannot bind the same slot. Inner completion may
propose retirement of unused slots, but the owning preparation releases them only after
proving their future dispatch paths are closed and no enclosing obligation still needs
them. Nested maximums are not added both as a group total and as member totals.

Next obtain durable reservations for every slot against the same tenant scope.
Sequential `reserve_envelope` calls are sufficient: the race funding gate stays closed and
**no slot may be armed or sent** until all required receipts are present and one
control turn validates the entire funding set. That turn opens the group funding gate;
each later participant still passes ordinary current dispatch/resource/authorization
checks. A partial reservation set never authorizes partial race execution.

If any reserve fails, is unknown, or exceeds its deadline, keep the gate closed,
reject/fail the race preparation before participant dispatch, and close all planned
delivery gates. Release confirmed unused holds only through revision-checked no-handoff
proof; query unknown reservations using their original IDs and retain unresolved
liability. No actual attempt or provider effect is fabricated to explain these holds.
Cancellation or policy revocation during preparation uses the same path.

The plan's finite participant/operation envelope assigns stable `FundingSlotId` values
to every permitted operation/attempt slot before reservation. They are budget identities,
not already active AttemptRecords or synthetic Jobs. If concrete inputs are available,
the envelope fixes their digest immediately. A bounded dynamic paid operation whose
input is not yet known reserves its maximum under the pinned capability/input/effect
envelope fingerprint. `bind_attempt` later fixes its concrete OperationId, AttemptId
and input digest before arming, without releasing the hold or widening authority.
All retry slots of the same logical operation bind that operation's same digest and
provider idempotency identity, each with its distinct actual AttemptId. Repeating the
same binding is idempotent; a changed binding conflicts and cannot arm. Unknown
expansion/maxima reject preparation rather than funding only initially visible calls.

Actual retry remains subject to the explicit retry policy and stopped/reconciled prior
attempt. It consumes its already-funded unused slot without a second budget charge.
Unneeded retry/dynamic slots stay held until that operation/group has irrevocably closed
its future dispatch gate, then release only as proven never armed/sent. A winner does
not release any armed or uncertain loser liability. Failure after full funding opens
the race follows ordinary child failure/cleanup semantics; it does not rewrite already
committed outcomes. Durable inventory includes every slot even if the Runtime crashes
before the funding gate opens.

## Local commitment and durable handoff

There is no distributed atomic commit across Runtime, authority and provider. Safety
comes from durable conservative liability before any possible provider send:

1. Prepare current Run policy, aggregate child/attempt limits, local resources and a
   finite provider-enforced maximum. Atomically reserve the maximum with the authority.
   Paid races must first complete the full funding barrier above; selecting one of
   those slots consumes its existing reservation without charging twice. No paid
   provider request is sent at this point.
2. Create and retain the one-shot DeliveryGate in `prepared` state, with bounded
   completion/cleanup storage, before submitting `arm_handoff` with its identity.
   Persist arming before the provider can receive a request. An acknowledgement
   returns the ticket bound to the operation/attempt/digest. Unknown reserve or arm
   response is queried by the same identity; it never causes a new reservation or send.
3. On the control executor, revalidate all current policy, concrete-input scope, deadline,
   capability pins, resource grants, budgets and ticket. In one dispatch turn commit
   the attempt/event group, change the gate to `dispatch_committed`, and prepare at
   most one bounded outbound work item for delivery after commit. Only the winner
   of that gate's send claim can spend the armed ticket. A closed gate prevents send;
   it cannot authorize another attempt or silently substitute new inputs.
4. The configured adapter validates the ticket/binding/expiry and atomically claims
   the gate before crossing the provider boundary. A failed claim sends nothing.
   A successful claim conservatively makes send possible, even before socket I/O;
   later cancellation cannot establish no-send. An
   `attempt.dispatch_committed` event is intent, not provider receipt or success.
5. Settle from authoritative provider/billing evidence. Run terminalization, output
   rejection, timeout, cancellation, race loss and Runtime destruction are not settlement.

A proven pre-handoff rollback closes the one-shot local delivery gate and invalidates
all queued sends for that ticket before asking `cancel_before_handoff`. The authority
releases a merely reserved record only if no arm can subsequently commit; revision/state
comparison arbitrates those commands. An armed ticket requires stronger matching
no-send proof from its still-live delivery owner. If that owner crashed or any send is
uncertain, retain the entire maximum and reconcile. Never infer no-send from absence
of a local event, socket response or provider result.

Worker callbacks reference IDs/generations and bounded evidence, not Run pointers. Late
reserve/arm success after local cancellation is routed to accounting rollback/reconciliation;
it cannot dispatch a cancelled Job. Superseded callbacks remain relevant to durable
liability even when their lifecycle mutation is stale. No new physical attempt overlaps
an unresolved earlier attempt. Provider-deduplicated status/retrieval is allowed only
under a verified contract proving it cannot start duplicate work.

## One-shot DeliveryGate ownership and arbitration

`DeliveryGate` is a bounded control block, not a Job or authority to spend by
itself. The Runtime delivery registry owns it from preparation, linked to the
concrete AttemptRecord/pending dispatch. On cancellation, failed dispatch or
terminal cleanup the registry transfers its ownership obligation to a matched
CleanupRecord before retiring the attempt linkage. This preserves the gate and
its immutable binding independently of Run observation retention. Adapter work
items hold strong references only to this gate, immutable ticket/input and a
bounded completion endpoint; they never capture Job/controller pointers.

This is a narrow exception to the usual immutable-only shared ownership rule:
a `shared_ptr<DeliveryGate>` protects lifetime while a single atomic state
arbitrates close/send. Identity/binding fields are immutable; no shared mutable
lifecycle fields or borrowed data live here. Control-side preparation/dispatch/
cleanup remains serialized by the executor. No lock is held across I/O and no
worker needs a control-thread round trip to close or claim the gate.

| State | Permitted transition and owner | Evidence |
| --- | --- | --- |
| `prepared` | Control executor changes to `dispatch_committed` in the final dispatch commit, only after a matching armed receipt; control may instead close it | Arm may be pending/unknown; worker cannot send |
| `dispatch_committed` | Adapter worker atomically claims `send_claimed`, or control/adapter atomically closes it | Exactly one compare-exchange wins; duplicate work items cannot claim twice |
| `send_claimed` | Winning worker records `sent_or_unknown` after transport attempt or failure | Already handoff-possible; never eligible for local no-send proof, including pre-I/O crash/failure |
| `sent_or_unknown` | No reopening or local no-send transition | Outcome/cost settle separately through authoritative evidence |
| `closed_without_send` | Absorbing; cannot dispatch, claim, rearm or reopen | Immutable identity-bound no-send proof may be issued |

Close is a compare-exchange from `prepared` or `dispatch_committed` to
`closed_without_send`, retried only while one of those states remains. Release/
acquire publication makes the immutable binding and state available to workers.
Control stop/revocation handling attempts closure before publishing its stop
commit; an adapter observing expiry/invalid delivery likewise closes before
returning no-send. The close/send compare-exchange is their linearization point,
not queue removal, an atomic stop hint, or the time a caller requested cancel.
If send wins first, stop still applies to lifecycle but liability remains held.
If close wins first, every queued or duplicated sender fails its claim. Once
claimed, the adapter must not transparently resend/retry the provider request;
a new physical attempt requires the normal reconciled retry protocol.

The authority validates a bounded `NoSendProof` from the configured trusted
adapter/Runtime identity, never from client/model input: DeliveryGateId,
incarnation, tenant/operation/attempt/reservation/digest binding and closure
state. The implementation can construct this proof only from the absorbing
closed state of the still-live registry/cleanup-owned gate. An armed record also
checks its stored gate binding. `cancel_before_handoff` durably closes/tombstones
that same binding so a delayed arm cannot recreate handoff permission. Revision
conflicts query the current record and repeat the same identity-bound closure;
they do not allocate a new gate or reservation. Unbound race-slot release instead
uses the funding owner's irrevocable future-dispatch closure proof; it cannot
fabricate an Attempt or claim that an unknown bound gate never sent.

Required failure paths:

- Arm succeeds but local policy/deadline/resource/event preparation denies
  dispatch: close `prepared`, issue no-send cancellation, send zero provider calls.
- Arm acknowledgement is lost, then cancel: close `prepared`; reconcile/close
  the original binding. Delayed arm success cannot advance the closed gate.
  Authority outage keeps liability held until the durable closure is acknowledged.
- Dispatch commits but enqueue fails or cancel wins before send claim: close
  `dispatch_committed`; queued duplicates cannot send and release is proof-backed.
- Send claim wins before cancel (even before I/O): no local refund; retain maximum
  until provider evidence or conservative authority reconciliation settles it.
- Runtime crashes or loses the gate without a durably accepted closure: local
  proof is lost; inventory retains unknown liability. No reconstruction from the
  absence of events. A closure already accepted durably remains closed on restart.

The registry/cleanup owner releases its gate only after all arm/delivery callbacks
and worker references are quiescent and accounting has acknowledged closure/
settlement or explicitly transferred unresolved debt to the durable authority.
Queue emptiness, destructor invocation or Run terminalization are insufficient.
Gate count/storage is charged before arm against finite attempt and cleanup
budgets; capacity exhaustion prevents arm. Destruction never sends or refunds.

## Durable record transitions

These are budget/accounting states, not Job states or a second scheduler:

| State / observation | Next permitted accounting action |
| --- | --- |
| Reserved, never armed, matching gate closed | Release maximum once through revision-checked no-handoff cancellation |
| Armed, still-live owner proves no possible send | Release only after proof validation and irrevocable local gate closure |
| Armed and send/result uncertain | Keep full liability held; bounded reconciliation only |
| Confirmed provider outcome, actual cost still unknown | Record outcome; continue holding full unknown cost bound |
| Confirmed final cost at/below maximum | Charge confirmed cost, release only proven unused remainder, mark settlement revision |
| Authority conservatively charges full maximum | Mark accounted-at-maximum; safe balance may be certified without pretending provider outcome is known |
| Late evidence after maximum accounting | Apply idempotent correction only under authority policy/evidence; never let a Runtime refund itself |
| Observed cost exceeds enforced maximum | Retain evidence, block affected integration/scope and report contract violation; do not hide overrun or claim the hard guarantee held |

Reservation TTL, disconnected requester and Runtime restart never refund unresolved
armed liability. Records/identity tombstones remain durable for at least the provider's
possible execution/billing/reconciliation and deduplication period. If those periods
cannot be finitely established, the authority retains unresolved accounting until an
explicit policy-approved reconciliation; it does not expire it into new capacity.

## Restart, uncertainty and availability

A restarting Runtime requests complete inventory for every affected hard-budget scope.
No new paid Run in that scope is admitted until the authority proves either reconciled
outcomes or full bounded liability accounting and a safe remaining balance. Incomplete
inventory, unknown scope epoch, unreachable authority, or unverified provider ceiling
fails closed. Old record discovery never reconstructs a Run, Job, Continuation or lease.

Reconciliation may outlive the original process and is authority-owned. Any Runtime
assistance is a bounded registered status operation with separate cleanup authorization,
deadline and cost reservation if the query itself is paid. It cannot blindly repeat the
original effect. Unknown outcome and unknown cost are independent: accounting the full
maximum resolves budget safety but does not prove success, failure or rollback.

If the authority becomes unreachable after a provider send, already armed liability
remains durable. Stop further paid handoff; retain local outcome evidence and retry only
idempotent accounting/status messages under bounded policy. Non-paid work does not gain
permission to use quarantined resource/concurrency merely because it has no cost.

Within retained Run observation, late matching settlement produces correlated
`budget.settled` / reconciliation evidence with the authority revision and increasing
Run sequence, without changing terminal intent. After the reserved observation window
closes, the durable ledger still settles; no expired stream is reopened. Public records
contain bounded logical identities/amounts, never provider credentials, raw invoices or
unrestricted tenant inventory. Budget inspection has its own current access check.

## Deterministic contract tests

The fake authority is a separate test object retained when a fake Runtime is destroyed
and reconstructed; it models durability, not a production database. Its controllable
mailbox exposes reservation/arm/settle linearization, revision conflicts, partial inventory,
outage, lost response and conservative full-maximum reconciliation. A fake provider
independently models receipt, bound enforcement, billing, deduplication and unknown outcome.

Assert A05, A23–A25, A33, A41 and A42, plus crash/failure injection immediately before
and after reserve acknowledgement, arm acknowledgement, local dispatch commitment,
provider receipt and settlement acknowledgement. Repeat every accounting callback and
verify no double charge/refund; race cancel with arm/send and verify full liability
unless no-handoff is proved. Run two Runtime fakes against one authority to verify tenant
aggregation. For A24, let a first race-slot reservation succeed and a later one fail,
time out, or lose its acknowledgement: assert zero `arm_handoff`, participant dispatch
and provider calls; only proof-backed rollback/reconciliation may occur. Also test full
funding followed by unused retry-slot release, late failed-preparation receipts, and
dynamic-slot binding/widening rejection without releasing/re-reserving liability.
Lose a `bind_attempt` acknowledgement and repeat its identity: recover the same
binding/reservation without consuming a second slot; a changed digest/attempt conflicts.
None of these tests require money, network or provider credentials.
