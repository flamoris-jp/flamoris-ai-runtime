# Execution Model

## Status and scope

**Phase A architecture contract; no execution behavior is implemented.** `MUST`
and `MUST NOT` below constrain later implementation. They do not freeze C++
types, backend APIs, or a transport schema.

The initial execution authority is one Runtime process. Its Runs, Jobs,
Continuations, leases, and request deduplication records are process-lifetime
state. A restart does not resume them. Persisted traces are evidence, not a
recovery log. Durable recovery and distributed ownership require a later design.

## Authority and ownership

| Concept | Owns | Must not own |
| --- | --- | --- |
| Run | Immutable admitted plan reference, caller context, cumulative limits, root Job, result/provenance, cancellation scope | Independent scheduler slot or durable Agent identity |
| Job | One stable Job ID, lifecycle, machine state, parent/children, deadline, attempts, results, optional suspended Continuation | Host-wide service lifecycle |
| Continuation | Resume description attached to exactly one waiting/paused Job | Independent queue entry, timeout, cancellation authority, execution lease |
| Inference Machine | Supported tokenization/prefill/decode/sampling progression and backend-state validity | Unchecked capability dispatch or separate Job lifecycle |
| Workflow Machine | Plan bindings, dependency readiness, control groups, bounded child proposals | Interpretation of raw Workflow JSON in the Scheduler |
| Scheduler | Selection of eligible **Jobs only**, subject to resource/policy admission | Plan mutation or ownership of resume state |
| Resource Manager | Leases, actual retained allocations, accounting, placement admission | Inferring memory release from a Job state change |
| Capability adapter | Versioned invocation/result contract and truthful external status | Run policy or neighboring service domain authority |

Every admitted Run has exactly one root Job, including a direct inference Run.
Every other Job has one parent Job in that Run; the ownership graph is an acyclic
tree. Dependency edges may cross siblings but never create another owner.
Detached/background children are excluded from the initial contract. A parent
cannot reach a terminal state while a child remains nonterminal. A race may
resume its parent before losers finish, but those losers remain owned children.

The Job controller is the sole committer of Job lifecycle changes. Machines,
timers, adapters, and the Scheduler submit proposals to it. A serialized Run
control order arbitrates conflicting proposals; this is a semantic requirement,
not a commitment to one OS thread. Resource admission across Runs is separately
atomic. Agent, Generation, GPU Node Manager, and product authorities remain as
defined in [Architecture](ARCHITECTURE.md).

## Immutable plan and execution identity

Compilation produces an immutable, versioned Execution Plan. It includes:

- workflow schema/revision and compiler semantics revision;
- normalized steps, dependencies, binding/output schemas, control policies;
- exact logical capability contract versions and registered adapter revisions;
- effective effects, required permission scopes, side-effect ordering;
- finite expansion, resource, time, cost, output, and trace bounds;
- potential suspension sites and bounded dynamic-dispatch policy.

A canonical plan fingerprint identifies these semantics; it excludes credentials,
live handles, Continuations, and authorization grants. Canonicalization rules and
the digest algorithm are Stage B decisions. Given the same input, capability
snapshot, compiler version, and static limits, compilation MUST produce the same
normalized semantics or rejection. Executing a plan must not silently resolve a
pinned capability to a newer contract or adapter. Unavailable/incompatible pins
cause rejection or failure; replacement requires recompilation and admission.

The Run records the plan fingerprint, runtime instance, root Job, caller scope,
policy decision references, and immutable input bindings. Current authorization
and resource availability are checked again at admission and dispatch. A cached
plan is never permission. An execution attempt has a separate identity from the
Job; retries cannot erase previous effects or budget consumption.

## Creation, dependencies, and bounded expansion

For static DAG execution, the Workflow Machine maintains bounded node states:
`blocked`, `ready`, `active`, `succeeded`, `failed`, or `skipped`. These are
dependency bookkeeping, not a second Job lifecycle. A capability node creates
at most its declared bounded Job set once its required bindings are available.
Its Job map and creation counters are committed before any child can dispatch.
Pure control/binding nodes may advance without an additional Job, but still
consume a bounded control-step budget. Cycles and dynamic loops are excluded.

Input values and service-owned handles MUST be schema-checked and size-bounded.
Bindings become immutable for an invocation. A late upstream result cannot
rewrite an invocation already admitted or an output already selected. Unselected
branches become `skipped`; required failed dependencies propagate a declared
failure, rather than leaving descendants blocked forever.

Inference may propose work at a runtime control point not enumerated as a
concrete suspension in the plan. A proposal is untrusted data. The kernel MUST:

1. Parse bounded typed inputs and resolve only a capability allowed by the
   admitted expansion envelope, using its pinned contract and adapter revision.
2. Validate effects, schemas, child ownership, dependency acyclicity, maximum
   depth, cumulative Job/proposal counts, fan-out, cost, and input/output bounds.
3. Compile a bounded child fragment, record its fingerprint and provenance under
   the existing plan envelope, then perform current admission authorization.
4. Atomically reserve cumulative budgets, register children, and suspend the
   owning Job before making any child dispatchable.

This creates an execution instance of an authorized envelope, not an in-place
rewrite of the immutable plan. No capability registration, new endpoint, new
permission, or expanded budget may be inferred from model output. Invalid
proposals produce a bounded typed error for a declared recovery path or fail the
Job. Repeated invalid proposals consume a finite proposal budget. No recursive
unbounded tool loop is permitted. Child deadlines never exceed ancestor limits.

## Suspend and resume without changing Job identity

Suspension is legal only at an advertised machine/backend safe point where
resumable state is valid and execution has quiesced. Backend preservation,
offload, and rewind are capabilities to verify during Stage B research.

The controller atomically commits the Job transition to `waiting` or `paused`
and its Continuation. The record contains the owning Job, machine, suspension
generation, resume point, wait condition, bounded bindings, opaque state
reference/version, requirements, and affinity. It contains no execution lease.
Backend acknowledgement of execution quiescence precedes lease release. Any
resident allocation remains accounted independently of the Continuation.

When a wait is satisfied, the controller validates owner/generation and commits
`waiting -> queued`, consuming the Continuation into a bounded **Job-owned
pending-resume payload** in the same operation. The same rule applies to resume
from `paused` when its wait is satisfied. The payload is neither a Continuation
nor another scheduler identity. A queue delay or unavailable resource MUST NOT
discard it or its state footprint.

Immediately before `queued -> running`, the controller rechecks cancellation,
absolute deadline, current authorization, pinned contracts, state validity, and
resource admission. Only after new capacity is granted does the machine consume
the payload into active state. A failed capacity attempt leaves the queued Job
and payload intact. Permanent invalidation fails the Job and cleans the payload;
it never silently starts a fresh inference session. Stale/duplicate completions
cannot resume a Job twice because the suspension generation is already consumed.

## Scheduling and bounded progress

A Job is eligible only when its bindings/dependencies are ready, its Run pause
gate is open, its not-before time has passed, and it has no termination intent.
Queued Jobs may wait for capacity without owning an active execution lease.
Logical concurrency never guarantees simultaneous physical execution.

The initial policy uses deterministic FIFO admission within a configured priority
class, a documented age bound against starvation, and stable Job identity
as the last tie-breaker. Physical placement considers real accounting. Later
residency optimization may reorder otherwise eligible Jobs only within fairness,
deadline, dependency, and effect-order constraints. Stage B chooses the concrete
queue algorithm and finite policy values.

Control progression and cleanup must have bounded reserved capacity independent
of child worker slots. A waiting parent must not consume the active-worker quota
needed by its child. Retained memory can still block that child: perform an
advertised safe offload, or fail with a resource/deadlock error. Never discard
needed state or wait forever for the child to free its own blocked parent.
See [Resource Model](RESOURCE_MODEL.md).

## Await and join

Every wait names a nonempty, finite, closed child set and an absolute deadline.
`await` is a one-child `all_success` join. The initial join policies are:

| Policy | Decision | Child handling |
| --- | --- | --- |
| `all_success` | Succeed only after all named children succeed; first committed required failure/cancellation selects failure | Cancel unfinished siblings, then settle the owned subtree |
| `all_settled` | Wait for all named children to become terminal; return a typed outcome for each in declared order | Failures remain explicit data; no implicit retry or skipped errors |

Empty joins, unknown policies, duplicate participants, and participants outside
the owned group are invalid. A group deadline never turns an unfinished child
into success or a fabricated settled result. Optional behavior is expressed by
an explicit `all_settled` result/branch, not by silently ignoring an error.

## Basic race

A race has a finite participant set, an overall deadline, bounded deterministic
acceptance rule, and a loser policy. The initial policy is
`cancel_unfinished`; allowing losers to run for caching and provisional result
replacement are deferred extensions. No new participant may join a started race.

Only a terminal successful candidate whose bounded output passes the acceptance
rule can win. Acceptance cannot perform I/O or invoke capabilities. The earliest
accepted candidate in the serialized Run commit order wins; batch ties use the
declared participant order. A single committed `race.winner_selected` fixes the
winner permanently. Failure cannot win; if all settle without acceptance, the
race fails with `race_no_acceptable_result`. Deadline expiry fails with timeout.

Initial races exclude `write` and `destructive` participants. `external`, `read`,
and `paid` still require normal policy checks; paid races reserve the maximum
permitted cost across **all** participants/attempts, not only the winner.
Dependencies and explicit effect ordering may serialize participants.

Winner selection precedes loser cancellation requests and parent readiness. The
parent may resume with the winner while loser cleanup proceeds under its ownership.
Loser output cannot replace the winner; retained metadata/output obey separate
bounds. Loser cancellation is not rollback. Uncertain remote loser outcomes and
costs remain visible even when the parent eventually succeeds.

## Completion, failure, and interrupts

The detailed state transitions and ordering are normative in
[State Machines](STATE_MACHINES.md). Their central rules are:

- A result becomes a candidate only after schema/output validation and required
  child results are known. Entering `finalizing` freezes that terminal intent.
- Accepted cancellation closes dispatch first. The Job enters `cancelling`
  until execution is stopped or safely fenced, then `finalizing`. A late result
  cannot resurrect it as successful.
- Deadlines use an injected monotonic clock, continue during pause/queue/wait,
  and are checked before dispatch and accepting completion. Once terminal intent
  is frozen in `finalizing`, its workload deadline cannot overwrite that intent.
  Cleanup has a separate bounded allowance; it cannot authorize more useful work.
- Finalization disposes resume/active state, settles children, and either frees
  resources with acknowledgement or transfers explicitly bounded quarantine
  records. Quarantined capacity remains unavailable. A terminal Run therefore
  means its local ownership obligations are settled, not that every remote
  system rolled back.
- Unknown remote outcomes are recorded separately from lifecycle state.
  Cancellation with unresolved remote outcome fails with `outcome_unknown`;
  deadline failure retains `job_timeout` plus unknown-outcome metadata.

Interrupt commands have a bounded command identity, requested and applied/rejected
events, and a defined safe point. `pause` preserves state only when supported;
`resume` validates it again. `stop` is an inference-specific graceful output
request only if the backend and declared output contract allow partial completion;
otherwise it is rejected. `cancel` requests termination without a success result.
Input injection is allowed only at a declared bounded resume/input slot, never as
an arbitrary mutation of live context. Graph patching and rewind remain deferred.

See [Failure Model](FAILURE_MODEL.md) for attempt retries, uncertainty, containment,
and stable error responsibilities. Trace replay does not execute this model again.
