# Authorization, Effects, and Execution Boundaries

## Status and ownership

**Phase A architecture contract; no runtime behavior is implemented.**
This document defines authorization semantics, not transport authentication or a C++ API.
See [execution](EXECUTION_MODEL.md), [resources](RESOURCE_MODEL.md), and
[events](EVENT_MODEL.md) for the corresponding authority boundaries.

The Runtime authorizes active execution under deployment policy and an authenticated
caller context. Agent goals, model output, Workflow IR, capability discovery, and an
Execution Plan are never permission grants. The Agent remains authoritative for its
identity and durable policy; the Runtime can restrict a request further, never expand it.

## Authority at each boundary

| Boundary | Required decision | Failure behavior |
| --- | --- | --- |
| Compile | Validate schema/graph/bindings and resolve trusted capability contracts; derive requirements | Reject invalid or unresolvable input; create no runnable work |
| Run admission | Authenticate context, authorize plan and inputs under current policy, enforce limits and reserve bounded bookkeeping | Reject without dispatch; cached plan confers no prior permission |
| Job dispatch | Recheck caller context validity, capability pins/availability, concrete input scope, cancellation/deadline, budget and resources | Do not cross execution boundary; fail or wait only for explicitly transient conditions |
| Retry dispatch | Repeat dispatch checks, then prove retry is permitted for the previous attempt's outcome | Refuse unsafe or uncertain repetition |
| Resume | Revalidate active context, plan pins, state accessibility, current backend/resource epoch, remaining budget and deadline | Deny resume; discard pending resume state through accounted cleanup |
| Result/event access | Authorize current requester for Run ownership, requested data/trace scope and retention | Redact or deny independently of historical execution permission |

Every dispatch includes the current policy check, including `pure` and `read` work.
External, write, destructive, and paid work additionally requires the configured
effect-specific policy immediately before adapter handoff. Checks apply to the concrete
resolved inputs, not only a capability name approved earlier.

Admission produces a revocable, bounded Run authorization context: subject/tenant,
delegation scope, permitted capabilities/effects/object scopes, policy revision,
expiration, limits, and approved confirmation references where policy requires them.
It is internal state, not an exportable bearer token. A disconnected transport does
not itself cancel a Run; expiration/revocation still prevents further dispatch/resume.

Authorization and dispatch commitment are serialized against local cancellation,
revocation, deadline, and budget decisions. A policy change observed before commitment
prevents handoff. A later revocation requests stop of active work at its supported control
point; it cannot recall an already accepted remote effect. Adapters must advertise this
boundary and cancellation limitations. No distributed atomic authorization claim is made.

Cancel, inspect, resume, and inject-input commands have separate permissions. Ownership
of a Run does not imply unrestricted trace access or the right to broaden its plan.
Revocation never disables bounded Runtime-internal cancellation, allocation release,
accounting, and approved reconciliation. Such cleanup authority cannot create new user
work or perform an unapproved compensating mutation.

## Pinned plans and capability changes

An Execution Plan identifies its IR/compiler contract revision and immutable capability
contract fingerprints: identifier/version, input/output schemas, effects, configured
adapter revision, and relevant control/idempotency declarations. Workflow/model-authored
metadata cannot override registry metadata. Endpoints and credentials remain in trusted
adapter configuration, outside the portable plan.

Admission and each dispatch/resume verify those pins against the active registry.
A changed pinned contract yields `plan_stale`; the Runtime must not silently substitute
a newer capability, widen effects, or reinterpret bindings. Unrelated registry entries
changing do not invalidate a plan. Temporary unavailability preserves the pin and may
wait only within an explicit bounded availability policy and the original deadline.

Credential rotation may preserve an adapter contract only when the same configured
authority and object scope remain valid. Credential revocation or a changed destination
requires current authorization and, when the pinned adapter contract changes, recompilation.
Recompilation is new validation/admission, not permission to replay completed steps.
Automatic live plan migration is outside the initial architecture.

## Effects and composition

Registry effect sets are non-empty sets of known values:

| Effect | Meaning |
| --- | --- |
| `pure` | Only declared immutable inputs and outputs; no ambient access, mutation, external authority crossing, or policy-relevant cost |
| `read` | Ambient/mutable-state read beyond declared immutable inputs |
| `write` | Persistent or externally observable mutation |
| `external` | Crosses a process/service/network authority boundary |
| `destructive` | Destructive mutation; requires `write` explicitly |
| `paid` | Potential monetary, billed-token, quota, or similar external policy cost |

`pure` must be alone. `read` and `write` may coexist. `external` and `paid` are orthogonal:
`{external, paid}` is valid without adding `read`/`write`. Unknown values, empty sets,
`{pure, write}`, and `{destructive}` are rejected, not silently repaired.
Purity does not mean zero CPU/memory cost or unrestricted access to input data.

Run/branch effect aggregation is a normalized composition, not literal set union:
remove `pure` from validated member sets, union all remaining effects, and use `{pure}`
only if the non-empty collection contained exclusively pure members. Thus pure resize
followed by external inference summarizes to `{external, paid}`, never
`{pure, external, paid}`. An empty graph is not assigned purity to bypass graph validation.
Optional branches and race losers contribute to the conservative potential summary.
Actual dispatches also retain their own effects for provenance and accounting.

Effects describe possible behavior; they grant no object access or permission.
A write-capable registration does not authorize every file/repository. Deployment
policy must separately authorize the operation, target scope, caller and concrete inputs.
Trusted adapters are responsible for honoring their declarations; effect metadata alone
is not a sandbox for hostile in-process code.

## Inputs, bindings, and dynamic children

Binding evaluation is limited to validated references and bounded declared transforms.
It cannot resolve arbitrary filesystem paths, network URLs, executable code, or secrets.
Service-owned handles are scoped to subject/tenant and object access, checked when
resolved and again at use if mutable access can change. A string produced by a model
is data, never authority to choose a new endpoint, credential or adapter.

Input resolution that needs an ambient read or external call is an explicit registered
operation with effects and authorization; it is not hidden work inside a supposedly
pure transform. Outputs remain untrusted, size/schema checked, and cannot launder
new capabilities or permissions into a downstream binding. Result injection follows
the same rules and cannot overwrite completed plan facts.

A plan may declare a bounded dynamic-child envelope for inference-triggered work:

- approved capability contracts and input schemas/object scopes;
- maximum effects and required confirmation policy;
- maximum child count, depth, outstanding work, attempts and output size;
- aggregate resource, paid-call/token and elapsed-time limits;
- allowed suspension sites/policy and parent failure/cancellation behavior.

Each proposed child passes ordinary validation, authorization and atomic budget
reservation before becoming dispatchable. Its authority is the intersection of the
Run context, envelope and current policy. It retains parent/Run provenance and cannot
reset a deadline or transfer work into an unbounded detached execution.
Calls outside the envelope are rejected; expanding it requires separately authorized
new work, not an implicit privilege increase caused by model text.

## Budgets, concurrency, and confirmations

Run budgets are authoritative in the process-lifetime Run ledger; hard tenant
paid-cost budgets require an external durable budget authority. They are never
independent per-Job counters. The external authority atomically reserves a bounded
maximum for each paid operation/attempt under a stable tenant, operation and
attempt identity before any provider handoff, and records the unresolved liability
durably. A local process ledger or retained trace cannot establish a hard tenant
budget after restart. The external ledger supplies a current remaining balance
and reconciliation status to admission/dispatch. If it is absent or unreachable,
paid work requiring a hard tenant budget fails closed; non-paid work may proceed
under its independent policy and resources.

Reservation of child/attempt slots, maximum cost, output/event bounds and required
resource accounting is one logical eligibility decision before dispatch. If local
admission fails after an external reservation, the durable reservation is released
only with proof of no handoff; an uncertain handoff keeps the liability reserved.
The provider must enforce the per-attempt upper bound when hard spending is promised.
On restart, unresolved paid operations or an incomplete ledger/attempt handoff
inventory block new paid admission in the affected tenant/budget scope until
provider/budget reconciliation proves an outcome or the durable authority
conservatively accounts the full bounded liability and certifies a safe remaining
balance. Unknown cost is never inferred to be zero. Reservations stay held or
charged until settlement, not released because a caller times out, a Job becomes
terminal, or the Runtime crashes. This is budget authority only: it neither
recovers Runs nor promises exactly-once external execution. See
[RESOURCE_MODEL.md](RESOURCE_MODEL.md) and [Failure Model](FAILURE_MODEL.md).

Every cost-bearing attempt needs a configured finite upper bound or provider-enforced
limit. Estimates alone cannot promise a hard external spending ceiling; providers that
cannot enforce the requested bound must be rejected by a hard-budget policy. Race
participants reserve independently against the same aggregate budget, including losers.
Retry consumes its own attempt allowance; cancellation does not refund completed work.

Where configured policy requires confirmation, it must bind the caller, plan/operation
identity, concrete target/input digest, maximum effects/cost, and validity interval.
Changing those fields invalidates confirmation. No generic confirmation grants ambient
authority, and this design does not require confirmations for every operation.

## Attempts, idempotency, and uncertain outcomes

Submission idempotency is scoped by subject/tenant, request kind, key and canonical
request digest. Within an advertised retention window, the same key/digest refers to
the same Run; a changed digest is a conflict. After expiry or process restart without
durable deduplication, exactly-once submission is not promised.

A Job may have bounded attempt identities; Job identity and provenance remain stable.
Retries are allowed only by explicit policy, within original deadlines/limits, with a
known safe outcome or a configured provider idempotency contract. No new physical
execution may overlap an unresolved earlier attempt; provider-deduplicated retrieval
of the same operation requires a contract that proves it cannot start duplicate work.
A provider key is
stable for retries of the same logical operation and includes the relevant request
digest/scope; it is not regenerated to bypass duplicate detection.

| Observed outcome | Permitted next action |
| --- | --- |
| Rejected before adapter handoff | Retry under current policy if the failure is transient |
| Confirmed no effect occurred | Bounded retry if the registered contract permits it |
| Confirmed success | Record result; do not replay to recover a missing local response |
| Outcome unknown after handoff | Reconcile via a registered read/status operation or proven provider deduplication; otherwise stop with `outcome_unknown` |
| Confirmed partial effect | Report partial provenance; compensation is separately authorized work |

Read/pure work may be retryable but still consumes budgets and requires current input
access. Paid work is not free to retry merely because it does not mutate user data.
Cancellation, deadline expiry, transport failure and race loss are not evidence of
non-execution. Unknown external cost/outcome remains conservatively reserved/accounted in the
durable paid ledger until settlement or explicit operator reconciliation,
including after Job termination or process restart.
Trace replay never enters these execution paths.
