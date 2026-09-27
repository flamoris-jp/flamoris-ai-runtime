# Authorization, Effects, and Execution Boundaries

**Scope:** one local user. Transport identity and capability authorization remain meaningful; tenant accounting and strict monetary limits are optional future profiles.

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
| Resume | Revalidate active context, plan pins, state accessibility, current native-worker/resource epoch, remaining budget and deadline | Deny resume; discard pending resume state through accounted cleanup |
| Result/event access | Authorize current requester for Run ownership, requested data/trace scope and retention | Redact or deny independently of historical execution permission |

Every dispatch includes the current policy check, including `pure` and `read` work.
External, write, destructive, and paid work additionally requires the configured
effect-specific policy immediately before adapter handoff. Checks apply to the concrete
resolved inputs, not only a capability name approved earlier.

Admission produces a revocable, bounded Run authorization context: single-user subject,
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
Service-owned handles are scoped to single-user subject and object access, checked when
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

The single-user baseline enforces finite per-Run execution, resource, child,
request, output and time limits in its process-lifetime ledger. It has no tenant
accounts, tenant isolation or hard monetary spending guarantee across crashes.
A paid external capability requires current effect-specific policy, registered
adapter, finite configured call/attempt envelope, and safe retry and
unknown-outcome handling. An estimate is not a spending ceiling. If deployment
requires a strict monetary ceiling, disable that capability until the separately
reviewed optional durable Paid Budget Authority profile is qualified. Its former
tenant reservation and restart semantics are retained only in the
[optional contract](phase-b/PAID_BUDGET_CONTRACT.md).

Reservation of child/attempt slots and resource/output/event bounds is one
eligibility decision before dispatch. Race participants consume aggregate Run
allowance, including losing and uncertain attempts. Cancellation does not refund
a completed effect. Where policy requires confirmation, bind caller, operation,
concrete target/input digest, maximum effects and validity period; a changed
field invalidates confirmation.

## Attempts, idempotency, and uncertain outcomes

Submission idempotency is scoped by single-user subject, request kind, key and canonical
request digest. Within an advertised retention window, the same key/digest refers to
the same Run; a changed digest is a conflict. After expiry or process restart without
durable deduplication, exactly-once submission is not promised.

After canonical request validation, the admission authority serializes the claim for
`(single-user subject, request kind, key)` with reservation of one Run identity as a single
atomic decision. The digest is stored with the claim. A concurrent same-digest caller
waits for the owner's admission decision and receives that same Run identity if admitted;
a different digest conflicts even while the first admission is pending. Only the claim
owner may create/dispatch work. If admission is rejected before a Run exists, all
current waiters observe the same bounded rejection, then the pending claim is released
after notifying them; a later submission may attempt admission anew. Once a Run is
admitted, its claim remains associated with that Run for the advertised retention,
including terminal failure. Dispatch or uncertainty never releases the claim early.
This process-lifetime atomicity does not make submission exactly-once across restart.

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
external provider outcome record where available; after a crash the baseline
cannot reconstruct a monetary ceiling or infer that unknown cost is zero.
Trace replay never enters these execution paths.
