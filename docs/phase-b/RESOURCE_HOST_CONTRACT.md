# Resource and Host Integration Contract

**Phase B design; no allocator, host adapter, or backend integration is implemented.**
The [Phase A resource model](../RESOURCE_MODEL.md) is the semantic authority.
This is a proposed C++ port contract, not a claim about an existing GPU Node
Manager API. Host/provider conformance must be demonstrated before enabling the
corresponding real integration. The deterministic fake implements this contract.

## Records and ownership

All identifiers below are typed values, never pointers or reusable native handles.
Resource Manager alone changes its instance-wide ledger on the single Runtime control
executor that also owns Run/Job control. Commands and acknowledgements
cross worker boundaries as bounded values; backend object destruction remains with
the backend owner until its work and callbacks have quiesced.

| Proposed record | Owner / lifetime | Required content and invariant |
| --- | --- | --- |
| `ResourceRequirement` | Immutable plan/segment value | Finite complete vector of RAM, device memory, execution slots, transfer/scratch, adapter concurrency, result/control storage; conservative growth bound |
| `HostEnvelope` | Resource Manager; valid only until its bounded freshness/ownership limit | Logical resource ID, authority revision, host epoch, permitted capacities, arbitration mode and enforceable revocation evidence; observation alone is not permission |
| `ReservationRecord` | Resource Manager; from successful atomic admission until consumed/released | Reservation ID, owning instance/Run/Job/attempt, vector, expected epochs, expiry and unmaterialized remainder |
| `ResourceTicket` | Resource Manager; one bounded dispatch preparation | References exact reservation and compatibility revisions; offers reserved capacity, not permission to execute |
| `ExecutionLease` | Resource Manager record, referenced by active Job/attempt | Lease ID, admitted vector and epochs, one current execution owner; no Continuation ownership |
| `AllocationRecord` | Resource Manager; until actual release/reconciliation | Allocation ID, backend incarnation, host epoch, pool/location, conservative bytes, shared references, quiescence/release evidence |
| `StateReference` | Job active state, Continuation or pending resume; cleanup after invalidation | Immutable allocation identities plus model/content/configuration and state revision; moving the reference never changes charged bytes |
| `CleanupRecord` | Resource Manager; may outlive the Run and its retained stream | Operation/allocation identity, old epochs, admitted maintenance allowance, uncertainty, bounded observation link and reconciliation obligations |

The ledger owns records; backend holders own physical objects. A shared allocation
has one record even across Runs. Each Run still reserves its full declared working-set
quota, including shared state it requires. An aggregate backend pool is charged once;
pool members may be tracked for sublimits without being added again to physical totals.
Reference release is a logical event. A zero reference count permits eviction but does
not itself prove physical deallocation.

Model residency shared beyond one Job has an independent `ModelResidencyRecord`.
Reserve its bounded physical footprint before materialization and count unique
allocations once. Per-Run working-set limits still include the resources a Run
requires. A last logical reference permits eviction but does not prove physical
release. Any future shared OpenCL process context needs explicit ownership,
bounded overhead and final-release evidence without making Runtime construction
permanently single-use.

## Proposed ports

Operations use typed results and operation IDs. Retrying a control message with the
same identity returns the same committed decision or current status; changed arguments
for that identity conflict. Every callback supplies instance, operation and applicable
attempt/allocation/host/backend generations. Missing or mismatched identity cannot
change a current grant. External operations run on workers; the control executor never
waits on a host/provider/backend call.

| Port / operation | Input | Result or acknowledgement |
| --- | --- | --- |
| `ResourcePort.reserve` | Current requirement, owner, epochs, quota/cleanup obligations | All-or-none `ResourceTicket`, or finite queue/rejection decision; no physical work |
| `ResourcePort.commit_dispatch` | Prepared ticket plus current Run eligibility decision | One consumption into an execution lease, or rejection without execution; no reuse of ticket |
| `ResourcePort.materialized` | Reservation, allocation identity, conservative measured/bounded size | Atomic reservation-to-allocation conversion and ledger revision; no double charge |
| `ResourcePort.abort_preparation` | Ticket and materialization/handoff evidence | Release proven unmaterialized remainder; materialized/unknown portions enter cleanup |
| `ResourcePort.quiesced` / `release_confirmed` | Matching operation and backend/host evidence | Release execution capacity / physical allocation separately, exactly once |
| `ResourcePort.transfer_to_cleanup` | Residue plus containment proof and reserved observation allowance | Acknowledged cleanup owner, or refusal that prevents terminal publication |
| `HostAuthorityPort.observe_envelope` | Logical resource, instance, supported arbitration mode | Fresh enforceable envelope or unavailable; no inferred host exclusivity |
| `HostAuthorityPort.acquire` / `release` / `reconcile` | Bounded operation, ownership token, expected epoch | Explicit acknowledged / rejected / unknown outcome and authority revision |
| Backend resource port | Load, grow, quiesce, offload, snapshot, evict, release; bounded request | Actual result/footprint and compatibility/quiescence/release evidence, independently advertised support |

Names are conceptual methods, not a public ABI or transport schema. The
[concurrency contract](CONCURRENCY.md) owns dispatch linearization. In the
baseline, one non-blocking control turn validates all grants and commits the bounded
event group; no domain mutex nesting is needed. Cross-Run decisions have a ledger
revision without exposing a global Run-event order. A prepared ticket is never
sufficient by itself for a worker to start.

## Dispatch and acquisition transaction

The transaction is a local eligibility decision with explicit external uncertainty,
not distributed two-phase commit:

1. Scheduler proposes an eligible Job. The controller checks current inputs, capability
   pins, authorization, deadlines and finite cumulative limits. Resource Manager
   reserves the entire incremental vector atomically, including cold load and transfer
   headroom. An unavailable vector grants nothing.
2. Bounded external capacity reservations are requested on workers and correlated
   to this preparation generation. They do not start inference or provider work. Failure
   cancels preparation; unknown acquisitions stay charged and are reconciled.
3. At the serialized dispatch commitment, repeat current cancellation/deadline/policy,
   capability/state/host freshness checks; consume the prepared grants together with
   child/attempt/cost/event eligibility. Publish no runnable worker command if any part
   fails. A successful commit records the attempt and exact lease/receipt identities.
   Race participants must fit the finite aggregate Run attempt/resource envelope.
4. Backend allocation/load occurs only within that complete envelope. Each acknowledged
   physical allocation consumes its reserved portion atomically; unexpected growth needs
   a new full incremental admission before allocating. If the backend cannot bound growth,
   reserve its safe maximum or reject that operation.
5. Useful inference or adapter work starts only when the required vector is materialized.
   A partial allocation failure releases confirmed pieces and retains uncertain ones.
   No partial acquisition waits indefinitely for missing capacities while running useful work.

Host revocation and a committed worker command follow the fencing rules below. A policy
change after the serialized commitment may request stop but cannot retract a completed
external effect. A future strict monetary profile, if requested, is separately documented in
[Optional Paid Budget](PAID_BUDGET_CONTRACT.md). Baseline physical vectors
are admitted per eligible Job and bounded at the Run/group level.

Capacity for each pool is `unique resident + unmaterialized reservations + uncertainty`.
Revoked/expired leases never subtract bytes. Resource observation with overlapping
Runtime allocations must supply a conservative non-Runtime allowance/envelope; adding
whole-device utilization to Runtime allocation totals would double count.

## Preservation and acknowledged release

| Operation | Required preparation | Successful acknowledgement means |
| --- | --- | --- |
| Suspend in place | Backend safe point with consistent sampler/token/cache state | Active execution quiesced; lease capacity releasable, retained bytes still charged |
| Offload | Reserve target and transfer capacity while source remains charged | Target state validated; source freed only on separate matching release acknowledgement |
| Snapshot | Advertised format/version, approved bounded storage, compatibility digest | Snapshot validated; source allocation still charged until release acknowledgement |
| Resume | State compatibility and current policy/epochs verified; new complete lease | Same Job activates preserved state; no automatic reconstruction or re-inference |
| Evict warm model | No required live reference, or proven preservation removed that dependency | Actual unload/release confirmed; no remaining pointer/worker access |
| Cancel/discard | Invalidate resumability first; keep cleanup ownership | Local work stopped/contained and each footprint released or explicitly quarantined |

During offload source, target and transfer staging coexist and count. Lost source-release
acknowledgement leaves both footprints charged even if the target is usable. An invalid
target never silently replaces a valid source. A failed evacuation is not reported as
successful pause-and-free. Unadvertised operations return unsupported without mutating
state. Offload/snapshot formats are backend-specific and not accepted as portable tokens.

Maintenance after suspension uses a bounded controller operation with owning
Job/cleanup provenance and separately reserved transfer/device capacity. It executes no
useful inference, owns no scheduler identity and cannot loan a lease to a Continuation.
If retained parent state prevents required child progress, attempt only approved and
supported bounded preservation; otherwise fail `resource_unavailable` with
`resource_deadlock`. Atomic admission alone does not detect this dependency deadlock.

## Epochs, containment and lifetime

The Resource Manager serializes envelope revision/revocation with local grant validation.
An integration must either enforce generation/ownership checks at the actual device
work boundary, or provide a host-enforced single-owner envelope whose withdrawal prevents
replacement ownership until old work is stopped/contained. A stale free-memory sample,
local mutex, callback discard, or timestamp expiry is insufficient. If neither mode is
verified, that resource is unavailable for real dispatch.

On unknown/stale ownership or epoch change: fence new dispatch/growth/resume on affected
resources, invalidate old permission, request bounded quiescence, and retain old
allocation/in-flight accounting. Only independently verified resources continue. State
whose compatibility cannot be proved becomes unavailable; it is not resumed under a
new model/device incarnation. A new envelope must account for or prove release of old
uncertainty before granting overlapping capacity.

A late callback may reconcile its *old* cleanup record when its full identity and proof
match. It cannot release a replacement allocation, renew an expired lease, resurrect a
Job, or validate newer state. Duplicate acknowledgements make no second subtraction.
Backend completion inboxes retain only bounded value messages and weak registry lookup;
physical backend holders survive until worker/callback quiescence. Never destruct an
in-process writer's memory because its callback generation was fenced.

Unstoppable native work is not safely contained by ignoring its callback. If isolation
cannot be proved, keep ownership and report stuck work to the configured supervisor;
do not manufacture terminal/quarantine success. Host reset/recovery remains a separately
authorized host action, never a Kernel resource-reclamation shortcut.

## Quarantine, observation and restart

Before transferring residue, reserve a bounded cleanup record, maintenance allowance and
post-terminal observation/closure capacity. Failure to secure them prevents terminal
publication via transfer. Quarantine counts against physical capacity and appropriate
concurrency until matching evidence resolves it; time-based record expiry cannot refund it.
Tracking exhaustion closes affected admissions, preserving reserved cleanup progress.

Within retention, a matching release appends a bounded same-Run reconciliation group with
increasing sequence and ledger revision after the terminal group. Terminal intent/results
remain immutable. Before the window closes, emit resolved or still-unknown closure from
reserved capacity. After closure, update the resource ledger without reopening the stream.
The cleanup record retains bounded provenance independent of any destroyed Run object.

On process restart there is a new instance identity and no reconstructed Job, lease or
Continuation. Host/backend reconciliation must establish a valid new envelope before
affected resource admission. Surviving allocation/in-flight uncertainty is not inferred
away from absence of local records. Optional strict paid-cost liability has a separate external authority when enabled.

## Deterministic conformance

Fake resource/host ports expose a manual command queue, revisions/epochs, bounded vectors,
partial acquisition, delayed/duplicate/stale acknowledgement, lost release, and enforceable
versus advisory fencing modes. Assert each ledger change and permit zero dispatch when
authority is advisory/unknown. Cover A08–A13, A19, A23, A28–A29, A33–A34 and activation cases
in [Activation Contract](ACTIVATION_CONTRACT.md). Real adapter tests separately prove
allocation bounds, stop/containment and host arbitration; fake success never certifies them.
