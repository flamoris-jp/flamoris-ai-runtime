# C++ concept, ownership and lifetime design

Status: Phase B proposal, 2026-09-27. These names are design responsibilities,
not production declarations or a stable public ABI. Phase A
[execution](../EXECUTION_MODEL.md), [states](../STATE_MACHINES.md),
[resources](../RESOURCE_MODEL.md), [authorization](../AUTHORIZATION_MODEL.md)
and [events](../EVENT_MODEL.md) remain semantic authority.

## Representation rules

Use `flamoris::runtime` for source API values and narrower internal namespaces
for implementation. Values crossing a boundary own their bounded data. Mutable
aggregates have one owner, normally `unique_ptr` or direct containment.
`shared_ptr<const T>` is allowed for immutable plans, registry snapshots and
bounded immutable result buffers; it is not permission for shared mutable Jobs.
Use local references/spans only inside a synchronous call whose owner remains
alive. Never retain `string_view`, JSON element references or backend buffer
pointers in a command or event.

The baseline has **one Runtime control executor**. It owns all mutable domain
state, including Run controllers, the Scheduler and Resource Manager ledger.
These remain separate logical authorities despite thread co-location. Workers
perform bounded compute/I/O and return value observations. An inbox mutex/CV
protects transport of messages, not domain state. See
[Concurrency](CONCURRENCY.md) for atomic commits, queue bounds and shutdown.

Table shorthand:

- **C**: only the Runtime control executor reads/mutates live fields; other
  threads get immutable snapshots or submit commands.
- **W**: a designated worker owns native operation state until it acknowledges
  quiescence; C receives observations, not mutable references.
- **I**: immutable after construction; share only within bounded retention.
- **wire**: serialize a versioned redacted value projection, never C++ memory.
- **internal**: no import/export of runnable/live objects; an inspection
  projection may carry non-authorizing identities.

Each identity is a distinct strong type, not interchangeable integers or
strings. `RuntimeInstanceId` is a freshly generated 128-bit process-incarnation
identifier. Per-instance Run/Job/operation counters and generation counters use
checked unsigned 64-bit storage; exhaustion rejects creation/dispatch before
wraparound. A Job identity includes its owning Run, and a Run includes the
runtime incarnation. Wire IDs are opaque bounded strings; clients must not
derive authority, ordering or future IDs from their spelling. Test ID sources
are injected. External durable tenant/operation identities have their separate
namespace and must not depend on a process counter alone.

## Runtime and execution mapping

| Concept → proposed type | Owner and lifetime | Mutation / valid references / thread | Serialization | Cleanup and invariants |
| --- | --- | --- | --- | --- |
| Runtime instance → `RuntimeInstance` | Composition root, creation through completed shutdown | C; owns executor, indices, workers/ports; exports command and observation interfaces | Instance ID/status wire; object internal | Stop admission, drain/contain, quiesce callbacks/workers before destroying ports; restart never recovers Jobs |
| Run → `RunController`, `RunRecord` | Runtime Run index; admitted lifetime plus bounded observation retention | C; owns Job tree, immutable plan/input references, budgets and gates; outside references are `RunId` | Status/result wire; record internal | Drop workload ownership only after all Jobs terminal and debt transferred; terminal intent immutable |
| Job → `JobRecord`, `JobController` | Exactly one Run controller; parent `JobId` except root; bounded post-terminal provenance | C; JobController is a mutation capability used only in Run commits, not a second owner; Scheduler keeps IDs | Status wire; live record internal | One lifecycle and payload alternative; descendants settle before parent terminal; stable ID across yield/resume/retry |
| Attempt → `AttemptRecord` | Job; one active attempt, bounded historical outcomes | C; backend/adapter messages name `AttemptId` and `DispatchGeneration` | Redacted outcome/effect evidence wire | A retry creates a new attempt only on the explicit Phase A retry path after the previous attempt is stopped and reconciled; pause/resume segments preserve attempt identity |
| Continuation → `ContinuationState` | Move-only suspended payload inside one waiting/paused Job | C; owner Job, suspension generation, resume point, wait set and state-reference IDs; no worker pointer | Internal; bounded inspection metadata only | Consume once into PendingResume or invalidate into cleanup; has no scheduling identity, execution lease or independent timeout |
| Pending resume → `PendingResume` | Move-only queued Job payload, until dispatch or cancellation | C; same resume/state identities transferred from Continuation | Internal | Retains memory/accounting while capacity unavailable; activate only after new dispatch checks; no new Continuation queue |
| Execution Plan → `ExecutionPlan`, `PlanStep`, `Binding`, `ChildEnvelope` | Compiler creates immutable object; Runs/cache hold bounded shared ownership | I; typed indices/IDs within the same plan; capability pins, no live handles | Canonical semantic export; not runnable-state import | Cache eviction releases immutable bytes only; never contains permission, credentials, KV or concrete Continuation |
| Plan fingerprint/version → `PlanFingerprint`, `PlanRevision` | Value embedded in plan/Run/provenance | I; compiler semantics + schema + normalization revisions and SHA-256 digest | Wire | Fingerprint is identity evidence, never an authorization token; comparison checks revision as well as bytes |
| Workflow Machine → `WorkflowMachineState` | Active coordinating Job payload | C; immutable plan, bounded node/group state, child IDs and typed bindings | Internal; bounded progress projection wire | Proposes ordinary child registration and lifecycle effects through JobController; no independent queue or terminal status |
| Inference Machine → `InferenceMachineState` | Active inference Job payload; native segment holder may be W | C controls stages; W owns exclusive native context access; state references identify ledger allocations | Internal; bounded stage/token projection wire | Every exposed token follows consistent state/sampler advancement; suspended state moves through Continuation; no blind reconstruction of missing cache |
| Scheduler → `Scheduler` | Runtime instance | C; bounded readiness queues of Run/Job IDs, not owning pointers or Continuations | Policy/status projection only | Removing a queue entry cannot release retained memory; selection is a proposal subject to final current checks |
| Capability Registry snapshot → `CapabilitySnapshot`, `CapabilityPin` | Registry publishes immutable snapshots; compile/Run pin applicable entries | I snapshots, C active pointer; logical adapter revision and schema/effect/control fingerprints | Redacted discovery/pins wire; endpoints internal | Retire snapshots after references expire; changed unrelated entries do not stale a plan; pins never grant invocation authority |
| Effect set → `EffectSet` | Value within validated capability/plan records | I after checked factory; closed known-bit mask rather than an unchecked enum cast | Canonical ordered string array | Reject empty, unknown, pure+other, destructive-without-write; aggregate only validated nonempty members |
| Authorization context → `AuthorizationContext` | Run controller, bounded until terminal; authenticated principal initially provided by transport | C; current subject/tenant scopes, expiry, decision/confirmation references; secrets excluded | Internal; redacted decision projection only | Revocation prevents new dispatch/resume but does not remove cleanup authority; never export as a bearer token |
| Authorization decision → `AuthorizationDecision` | One pending commit; bounded historical audit projection | I result with policy revision, concrete input/operation digest and scopes; C validates currency | Redacted wire | A previous allow cannot authorize a later dispatch/retry/resume; discard if relevant local revision changed |
| Submission claim → `SubmissionClaim`, `SubmissionIndex` | Runtime admission index; pending bounded waiters then advertised dedup retention | C; scoped key/digest and reserved RunId; waiters use response channels, not Run pointers | Response wire; index internal/non-durable | Atomic key+ID claim; failed pre-Run admission shares rejection before removal; admitted claim not released by failure/timeout |

`JobRecord` uses a checked payload sum: initial input, active machine,
Continuation, pending resume, cleanup-only references, or terminal metadata.
The transition function validates the payload-state pair against the complete
Phase A table, including `cancelling` and `finalizing`; merely setting an enum
cannot create a legal state. Moving payloads is ownership transfer, never a
copy of native inference state. Parent references and dependency edges are
non-owning IDs resolved inside the owning Run, avoiding C++ ownership cycles.

## Resource and observation mapping

| Concept → proposed type | Owner and lifetime | Mutation / valid references / thread | Serialization | Cleanup and invariants |
| --- | --- | --- | --- | --- |
| Resource Manager → `ResourceManager` | Runtime instance; ledger survives individual Run retention | C; owns reservations, leases, allocations and cleanup records; only it changes local accounting | Redacted ledger snapshots/revisions wire | Never infer host exclusivity or physical release from an expired C++ object |
| Resource requirement → `ResourceRequirement` | Immutable plan/dispatch proposal | I; bounded vector of resource classes, peak increments, affinity and host generation constraints | Semantic plan wire | Requirements are not grants; validate checked arithmetic before admission |
| Host envelope → `HostEnvelope` | Resource Manager; validity interval/generation | C replaces from authenticated host observations; references stable host grant identity | Redacted grant metadata | Expiry/revocation blocks affected useful work; the host authority, not the Kernel, controls other services |
| Model residency → `ModelResidencyRecord` | Resource Manager, load request through acknowledged release | C bookkeeping absent/loading/resident/releasing/unknown; existing admitted Job/attempt owns load operation, W owns native holder | Redacted status only | No scheduler identity; shared consumers wait until residency confirmed; loading/resident bytes stay charged |
| Admission gate → `AdmissionGate` | Runtime instance through shutdown | C; configured accepting/draining/closed state; external callers submit value requests | Status projection only | Closing admission does not terminate a native writer or free any resource |
| Process activation → `ActivationRecord` | Optional host activation gateway outside Kernel, bounded cold-start/idle-shutdown operation | Host authority owns ActivationKey/generation and process status; Kernel observes readiness through ports | Host-scoped status projection | Not a Run/Job or Kernel-owned process control; submit permission cannot grant host activation |
| Reservation → `ReservationRecord` | Resource Manager, prepared admission until conversion/release | C; ReservationId, incremental vector, owner and epochs | Observation only | Convert reserved-to-allocated atomically without double counting; uncertain materialization remains charged |
| Prepared resource ticket → `ResourceTicket` | Resource Manager plus bounded pending dispatch | C; handle to prepared grant with ledger revision/generation | Internal | Stale ticket cannot dispatch; ticket alone is no execution permission |
| Execution lease → `ExecutionLease` | Resource Manager; linked to one admitted active segment | C; LeaseId and generation, worker carries only non-owning operation token | Observation only | Release only on matched quiescence/containment evidence; lease release does not free resident allocations |
| Allocation → `AllocationRecord` | Resource Manager until acknowledged physical release | C ledger, W owns actual backend allocation holder; refer by AllocationId and epoch | Redacted physical-accounting projection | Shared allocation counted once; last logical reference schedules cleanup, never decrements physical bytes by itself |
| State reference → `StateReference` | Move-only Job payload descriptor; references Resource Manager allocation/state record | C moves descriptor; backend validity identified by model/config/backend/state generations; W accesses native holder only under grant | Internal; no native pointer or serialized KV | Continuation→pending→active preserves allocation identity; loss of validity fails resume and transfers cleanup |
| Quarantine/cleanup → `CleanupRecord` | Resource Manager after explicit transfer; may outlive Run stream | C; CleanupId, operation/allocation/epoch, bounded evidence and reconciliation authority | Redacted debt/reconciliation projection | Remains charged until matched proof; terminal Job cannot receive new state/results; expired stream never stops ledger cleanup |
| Paid Budget Authority adapter → `PaidBudgetAdapter` | Runtime composition root, pending calls through shutdown | External I/O W; C receives typed durable receipt/settlement with ledger revision and stable identities | Protocol values only; adapter/internal credentials never wire | Reservations belong to external durable authority; adapter destruction or Runtime restart cannot release liability |
| Durable receipt → `PaidReservationReceipt` | C attempt bookkeeping plus external authoritative ledger | I evidence: TenantBudgetId, OperationId, AttemptId, DurableReservationId, maximum liability, revision | Redacted receipt reference | Validate tenant/attempt/payload binding; unavailable/uncertain receipt prevents paid handoff |
| Paid race funding → `RaceFundingPreparation` | Run controller, from queued coordinator preparation until group future-dispatch gates close | C; fixed participant/operation/attempt-slot maxima and durable receipts; nested groups reference disjoint funded subsets | Internal; bounded funding status only | No participant dispatch before the full group is funded; no independent scheduling identity or simultaneous physical-capacity promise |
| Future paid liability → `FundingSlotId` | External durable reservation plus bounded Run bookkeeping | I identity/envelope; authority binds once to concrete operation/attempt/input through revision-checked `bind_attempt` | Redacted reservation protocol values | Unbound slots are not live Jobs/Attempts; bind/retry/nesting cannot debit twice; unused release needs irrevocable no-handoff proof |
| Event envelope/group → `EventEnvelope`, `EventGroup`, `EventGroupBuilder` | C builder before commit; bounded Run observation buffer owns immutable groups after commit | C allocates seq/group IDs; I committed group, bounded copies/shared const projection | Versioned wire | Reserve full group capacity first; commit state/group together; subscribers never mutate live state; retire complete groups |
| Failure/error → `ErrorEnvelope`, `ErrorCode`, `Result<T>` | Owned value at request/Job/result boundary | I once emitted; bounded safe cause-code list and authorized references | Versioned wire | Expected failures are values; sanitize before events/queues; external outcome independent of local failure |
| Clocks/deadlines → `MonotonicClock`, `Deadline`, `CleanupDeadline` | Runtime injects clock; Run/Job owns absolute deadline values | Clock read on C for arbitration; monotonic offset values; no system clock ordering | Durations/offsets wire; native time points internal | Child≤ancestor, queue/pause/retry never reset deadline; cleanup has separate budget; checked duration arithmetic |
| Cancellation/control → `ControlCommand`, `CommandReceipt`, `StopIntent` | Bounded inbox then owning controller; dedup record retained per policy | Immutable command values; C commits request/application and first stop reason | Command/receipt/events wire | Accepted request is not stopped/terminal; stop intent cannot be replaced by late completion |
| Callback delivery → `CallbackTicket`, `OperationObservation`, `CompletionSink` | Runtime sink plus worker-owned ticket; until worker/callback quiescence | W submits owned values; weak sink entry may fail closed; C validates all identities/generations | Internal | No raw Run/Job capture; stale lifecycle observations may still carry matched cleanup evidence |
| Retained observation → `RunObservationRecord`, `ObservationCursor` | Runtime until advertised stream closure/retention expiry | C appends bounded reconciliation, I snapshots with watermark | Wire | Terminal does not mean EOS; closure capacity reserved; no late append after stream closure |

Detailed ports and acknowledgement preconditions are in
[Resource/Host](RESOURCE_HOST_CONTRACT.md), [Paid Budget](PAID_BUDGET_CONTRACT.md)
and [Activation](ACTIVATION_CONTRACT.md). These tables do not replace those
protocols with smart-pointer reference counts.

## RAII, callbacks and failure-safe destruction

RAII owns local memory and native wrapper lifetime. It must **not** imply
successful asynchronous cancellation, release of a device lease, remote rollback
or durable budget settlement. An explicit close/cleanup request produces a
matched acknowledgement or a CleanupRecord. Destructors are non-throwing and
do not block on remote I/O. A native holder that is still writable by a worker
cannot be destroyed; it remains retained/contained, or the configured supervisor
terminates the process when safe local containment is impossible.

`CallbackTicket` is a discriminated value: pre-admission replies carry the
instance, submission-claim and operation generation, never fabricated Job or
attempt IDs; lifecycle observations carry instance, Run, Job, attempt/dispatch,
operation and applicable suspension/backend/host/allocation generations;
cleanup replies carry independent CleanupId/operation/allocation epochs, with
Run provenance optional after retention expiry. A weak sink reference may
extend only the mailbox endpoint lifetime during enqueue, never the Run.
Every enqueued observation owns its payload and a prepaid bounded delivery slot.
The control executor looks up the live owner and validates epochs. Invalid
lifecycle data cannot resurrect it; release/cost evidence is routed to matching
ledger records. Closing a sink without a worker-quiescence protocol is forbidden:
rejecting a callback does not prove the worker stopped touching native memory.

Runtime shutdown closes admission, fixes the drain deadline, stops remaining
work, transfers accounted debt, stops/join workers when possible, seals callback
sources and drains their final messages before destroying the sink/ports. Ledger
cleanup may outlive a retained Run but never its containing process; on restart
the host/budget authorities establish a new safe envelope. There is no C++
serialization trick that restores a previous executor or live pointer graph.

## Test seams and representation gate

Inject `MonotonicClock`, ID source, manually stepped control executor, backend,
capability, host, authorization and durable-budget ports. An immutable inspection
snapshot exposes ownership IDs, generations, state/payload tag, resource vector
and event watermark; it exposes no mutators. Tests may select which queued
observation is delivered next before a commit, including stale/duplicate and
post-terminal observations. Worker-thread stress tests verify lifetime safety;
offline manually scheduled acceptance tests verify semantics.

Before a new long-lived type is added in Phase C, identify its one owner,
destruction precondition and callback fence in this mapping. Adding a shared
mutable pointer or asynchronous destructor as a convenience requires design
review. The architecture has no independent Continuation owner, no Scheduler
reference that keeps a Job alive, and no observer capable of dispatching work.
