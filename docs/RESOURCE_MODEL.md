# Resource Model

## Status and scope

**Phase A architecture design; no allocator, scheduler, or backend is implemented.**

This document defines resource invariants for one Runtime instance and one authoritative ledger.
It does not define a distributed scheduler, a GPU Node Manager API, or a concrete device backend.
Job transitions follow [State Machines](STATE_MACHINES.md); execution ownership follows
[Execution Model](EXECUTION_MODEL.md). Exact interfaces and synchronization belong to Phase B.

## Authority

| Authority | Owns | Must not infer |
| --- | --- | --- |
| GPU Node Manager / configured host authority | Host-wide device/service transitions and the host's permitted Runtime operating envelope | Runtime Job completion or successful preservation of inference state |
| Runtime Resource Manager | Instance resource ledger, reservations, execution leases, allocation accounting, quarantine | Host-wide exclusivity from a local lock or stale free-memory sample |
| Scheduler | Placement/order of eligible Jobs within admitted resources | A second lifecycle for Continuations or authority to override host policy |
| Backend/adapter | Actual allocation, state validity, quiescence and release acknowledgement | Permission to increase allocation beyond its admitted envelope |
| Job owner | State/control decisions and lifetime of resume-state references | Memory release from cancellation, suspension, or terminal status alone |

Host transitions use the registered host-control integration; Runtime must not bypass it with
service commands or reset a device to satisfy a Job. The integration contract must be verified
before implementation. This document does not claim the neighboring service implements fencing.

If multiple processes can allocate on a device, the host integration must supply a valid ownership
or capacity-arbitration mechanism. A single Runtime ledger cannot arbitrate other processes.
The initial supported deployment may instead require a host-enforced single-owner envelope;
an unenforced assumption of exclusive access is insufficient.

## Separate records, separate meanings

| Record | Meaning and lifetime |
| --- | --- |
| Resource requirement | Bounded demand for an execution segment: memory, execution slots, concurrency, and transfer headroom; capability metadata supplies bounds |
| Reservation | Incremental capacity promised but not yet materialized as a physical allocation; an admission may fail without performing work |
| Execution lease | Permission to execute within an admitted vector and current generation; belongs to the active Job/attempt, never to its Continuation |
| Allocation | Unique physical resident allocation or conservative backend-owned pool, with location, generation, size bound, references and release status |
| State reference | Reference to one or more allocations or validated preserved state; copying a reference does not allocate or release memory |
| Affinity | Soft preference for a model/device; cannot preserve rights, stale handles or availability |
| Quarantine record | Resource/operation whose release or completion cannot be proven; bounded tracking survives Job terminalization |

Model weights shared by two Jobs have one allocation record. Both Jobs may reference that record;
their private KV/state allocations remain separate. If a backend reports only aggregate pool size,
account the pool once and enforce internal sublimits; do not add its member sizes to the same
physical total. Shared-allocation lifetime ends only when all required references are released
and actual eviction is acknowledged.

A model identity includes the relevant revision/backend configuration and device generation.
A similar name or warm-model hint is not evidence that preserved state is compatible.

## Capacity accounting

For each memory pool, admission must keep the following within its current permitted envelope:

`unique resident allocations + unmaterialized reservations + uncertainty/safety allowance`

Resident allocations include active state, retained suspended state, cached models, transfer
buffers and quarantined allocations. Conversion from reservation to allocation is an atomic
ledger change, not an additional charge. Releasing an execution slot does not subtract bytes.
Similarly, a reported external-device utilization sample already containing Runtime allocations
must not be added wholesale to the Runtime ledger. The adapter must identify the non-Runtime
allowance or provide a conservative envelope; unresolved overlap makes capacity uncertain.

Physical feasibility and per-run quotas are separate checks. Shared allocations are charged
once physically; each Run must still satisfy its declared working-set quota, including shared
state it needs, so shared models cannot bypass tenant/run limits. Backend-private memory without
reliable subdivision requires conservative bounds. Estimates are not guarantees against OOM.

RAM, VRAM, transfer staging, scratch storage where enabled, execution slots, provider concurrency,
and bounded event/result storage are distinct resources. Monetary/token budgets follow
[Authorization Model](AUTHORIZATION_MODEL.md) and are not made available by releasing memory.
Growth beyond admitted bounds requires another admission before allocation; otherwise fail the
operation with a resource error. A backend unable to enforce growth must reserve its safe maximum
or advertise the operation unsupported under that envelope.

## Admission and release protocol

1. Determine that the Job is dependency-ready and still runnable under current policy.
2. Validate resource requirements, preserved-state compatibility, budgets, capability availability
   and current host/backend generations. Affinity does not replace these checks.
3. Reserve the complete incremental resource vector atomically in the instance ledger, including
   transfer headroom and any capacity needed to load a cold model. Failure reserves nothing.
4. Recheck cancellation/deadline and dispatch authorization at the dispatch boundary; issue the
   execution lease only for this Job/attempt and generation. If dispatch is aborted, roll back
   unmaterialized reservations. Materialized allocations require confirmed release or quarantine.
5. Materialize allocations through the backend and convert their reservations without double
   counting. A failed partial acquisition unwinds confirmed pieces; uncertain pieces remain charged.
6. Start or resume useful segment work only after the complete required vector is available. No running Job retains
   some newly requested execution capacities while waiting indefinitely for the remainder.
7. On completion/suspension, request quiescence and release. Record acknowledgement and ledger
   changes independently; logical completion, an expired lease and a sent release request are
   never sufficient evidence that hardware work or resident allocations have stopped.

External capacity acquisition is not a distributed atomic transaction. Reserve locally, make a
bounded external acquisition attempt, and roll back on failure; no external operation starts
while waiting for the remainder of a multi-resource vector. Uncertain external reservations or
in-flight work remain quarantined/accounted rather than silently returned to the pool.

Resource, child-count and cost reservations participate in one committed dispatch-eligibility
decision with authorization; no component may spend an independently approved partial grant.
Bounded release/reconciliation authority and its cleanup allowance survive execution revocation;
they cannot be used to start new useful work or bypass a revoked capability permission.

Required preserved state may already occupy resources before resume or child dispatch. This is
not a newly acquired partial execution lease. The scheduler must explicitly detect whether that
retained state makes the dependency impossible to run; it must not rely on atomic admission alone
to solve such a resource dependency deadlock.

## Suspend, preserve and resume

| Operation | Preconditions and accounting result |
| --- | --- |
| Suspend in place | Backend reaches a declared safe point and stops active work; release execution capacity after acknowledgement, retain model/state bytes |
| Offload | Reserve target and transfer headroom before copying; source and target coexist and are counted during transfer; free source only after validation and release acknowledgement |
| Snapshot | Require advertised state support, bounded approved storage and compatibility metadata; snapshot existence alone does not free source state |
| Evict warm model | Only when no required live-state reference depends on it, or a verified preservation protocol explicitly removes that dependency; charge until release acknowledgement |
| Discard required state | Invalidates resume; fail the owning Job according to its failure policy, never relabel it a successful pause |
| Resume | Validate preserved state and current policy/generations, acquire a new complete execution lease, then restore and continue the same Job |

A Continuation contains no lease. On wake, its payload may be consumed into the queued Job's
pending-resume state as defined by the state machine. Its allocation references and accounting
remain intact during queue/resource wait and move to active state only on successful resume.
Cancellation removes resumability but transfers outstanding release work to cleanup records.

Pause does not promise free VRAM or immediate pause application. If a caller requires both
preservation and evacuation of a device, support must be advertised and capacity for the transfer
must be admitted. Unsupported preservation/offload returns an explicit unsupported action; Runtime
must not destroy state and pretend that reconstruction or re-inference is an equivalent resume.

If a waiting parent's retained state prevents its required child from running, try only an
already permitted, supported and bounded offload/preservation action. Otherwise fail the blocked
execution with a resource-dependency reason and cancel/settle its remaining children under the
execution model. Do not wait forever, forcibly evict required state, or repeatedly restart inference.

This failure uses `resource_unavailable` with bounded reason `resource_deadlock`; the error taxonomy
belongs to [Failure Model](FAILURE_MODEL.md). Offload/cleanup after suspension is a bounded control
operation charged to admitted maintenance capacity, with owning Job/cleanup-record provenance.
It must reserve competing device/transfer capacity before acting, cannot run useful inference,
and does not put the Continuation in the scheduling queue or give it an execution lease.

## Freshness, revocation and fencing

Resource observations carry an origin, generation/epoch and freshness bound. Admission rejects
unknown, stale or inconsistent required host/backend state. A free-memory estimate does not grant
authority. The Stage B integration must define which authority can fence old dispatch and how.

Every local dispatch, backend completion, state reference and release acknowledgement is correlated
with its instance/attempt and relevant allocation/device generation. Late acknowledgements cannot
free a replacement allocation or validate state created under a newer generation. A raw address
or reusable backend handle is insufficient identity.

When a host epoch changes, ownership expires, or the authority becomes unknown:

- fence new dispatch, growth and resume on the affected resource;
- invalidate old execution permission and request bounded safe quiescence of affected work;
- retain allocation/in-flight accounting until authoritative reconciliation proves release;
- fail affected resumability if state is lost or its compatibility cannot be established;
- continue unrelated work only where its independently verified resources/policy remain valid.

Revoking permission does not prove physical preemption. If a backend cannot stop at a safe point,
its capacity remains unavailable/quarantined. Runtime must not overlap a replacement worker merely
because the previous worker missed a timeout. Host recovery/reset remains host authority's decision.

## Ordering, fairness and residency

The baseline scheduling order is deterministic among eligible Jobs: configured effective priority,
then enqueue order with Job identity as a final tie-break. Priority policy must include bounded
aging or equivalent admission fairness; exact policy values are deployment configuration.
Deadlines use the Runtime's monotonic clock and continue during queueing, suspension and cleanup
requests. Expiry stops useful dispatch; it does not erase cleanup obligations.

Later residency optimization may reorder only otherwise-ready work inside its allowed fairness
window. It must preserve dependencies, explicit effect order, priority/aging guarantees, budgets,
cancellation and deadlines. Warm work cannot continually bypass an older cold Job. At the fairness
bound, Runtime stops admitting conflicting warm work and makes a bounded attempt to drain/reclaim
eligible capacity for the older Job; infeasible capacity ends in an explicit resource/deadline failure.
Scheduling cannot guarantee a deadline where actual capacity is unavailable.

Reordering `A, B, C` to reuse the model of `A` and `C` is allowed only if `C` is already eligible
and has no dependency/effect ordering through `B`. A linear Workflow edge is never removed for
performance. Cost estimates, cache hits and affinity are advisory, with event provenance.

## Failure and cleanup

| Condition | Required outcome |
| --- | --- |
| Admission does not fit | Queue within bounded policy if feasibility may change; reject/fail impossible requirements; no partial execution |
| Allocation OOM after admission | Stop the failing attempt, reconcile actual allocations, emit resource failure; do not silently retry a side effect |
| Partial transfer or unload failure | Preserve validated source if possible; retain all uncertain allocations; no successful evacuation claim |
| Worker crash / lost completion | Fence its generation, mark state unavailable, account outstanding resources in quarantine and reconcile through backend/host authority |
| Remote cancellation cannot be confirmed | Retain operation uncertainty/concurrency debt; do not infer remote completion or refund consumed cost |
| Cleanup acknowledgement arrives late | Apply only to matching live cleanup identity/generation; never mutate a terminal Job back to runnable |
| Quarantine reaches its tracking/capacity bound | Stop affected admissions and require recovery/reconciliation; never expire records merely to make capacity appear free |

Run terminalization requires all Jobs terminal and run-owned resources either released or explicitly
transferred to bounded quarantine tracking. Quarantine keeps capacity unavailable until proof of
release and exposes cleanup status separately from success/failure/cancellation. A successful Job
does not turn cleanup uncertainty into free capacity.

The Job/Run `finalizing` stage settles this release-or-quarantine obligation before terminal
publication. Transfer to quarantine is confirmed containment and tracking, not confirmed deallocation.

A Runtime process restart loses active execution/Continuation authority in the initial scope.
It must start affected resources unavailable until reconciliation establishes a new valid envelope;
it must not reconstruct leases from event replay or assume its old allocations were freed.
Durable cross-process recovery is a separate future design, not an implied guarantee here.

## Required design acceptance scenarios

| Scenario | Expected observable invariant |
| --- | --- |
| Two Jobs share one warm model | One physical model allocation, separate state allocations, both Run quotas checked |
| Inference yields while KV remains resident | Lease release acknowledged; retained KV still charged; child admitted only if capacity fits |
| Parent KV plus child working set exceeds capacity | Supported offload with target headroom or explicit resource-dependency failure; no hang or fake free |
| Offload fails after copying | Source/partial target accounted until respective acknowledgements; preserved state validated before any resume |
| Resume queues behind another Job | Same Job identity, no lease in Continuation, pending-resume footprint continues to count |
| Deadline races with worker completion | No fresh useful dispatch after expiry wins; late completion settles resources without reviving the Job |
| Old unload callback arrives after device restart | Generation mismatch prevents release of the new allocation |
| Terminal Job has unknown GPU cleanup | Terminal outcome and cleanup debt are separately visible; replacement admission cannot consume uncertain capacity |
| Warm Jobs arrive continuously | Bounded fairness prevents indefinite residency-based bypass |

Resource events distinguish reservation, lease, allocation, transfer and quarantine changes; report
requested versus acknowledged operations separately. The event contract is defined in
[Event Model](EVENT_MODEL.md). Public status uses bounded logical resource summaries, never raw
device handles, credentials, private paths or host topology.
