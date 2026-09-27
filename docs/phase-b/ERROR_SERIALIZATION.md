# Error, wire representation and deterministic compilation

Status: Phase B proposal, 2026-09-27. No parser, schema files, compiler or
transport is implemented. This document chooses implementation encodings for
the semantics in [Workflow IR](../WORKFLOW_IR.md),
[Failure Model](../FAILURE_MODEL.md) and [Event Model](../EVENT_MODEL.md).

## Typed results and exception boundaries

Use an internal `Result<T>` facade with exactly one owned value or `ErrorEnvelope`;
the C++20 implementation is a checked discriminated union, with a unit success
value for `Result<void>`. Constructors/factories validate bounded data before
publishing it. No default-success result, null-as-error, unchecked enum cast or
exception-text parsing is permitted. Operation acceptance, observation delivery,
and final Job outcome are distinct result types; accepting a cancel command is
not returning a cancelled Job result.

Expected validation, policy, capacity, native worker, adapter, timeout and cancellation
outcomes travel as typed values/observations. Every worker entry and third-party
boundary catches exceptions and converts safe known categories; exceptions do
not cross callbacks or the C++ source API. Unexpected exceptions become
`internal_error`, or `native_execution_failure`/`upstream_failure` when known to originate
there, retaining dispatch/effect evidence. Never emit `what()` directly.

The controller prepares all allocations/event capacity before its commit. The
commit path uses checked non-throwing moves and cannot leave state committed
without its complete event group. `bad_alloc` before commit rejects the proposal
using reserved emergency error capacity. Exhaustion/corruption that makes even
this guarantee impossible stops admission and enters fail-stop containment;
do not continue a half-committed Run. A violated internal invariant is an
`invariant_violation` and quarantines affected execution; if ownership safety
cannot be established, escalate to the configured supervisor. Debug assertions
supplement these runtime checks and are never the only fail-closed behavior.

| Source condition | Stable category / code | Outcome and retry rule |
| --- | --- | --- |
| Invalid version, unknown field, malformed/oversized request | Input/plan: `invalid_request` or `invalid_workflow` | Reject before admission; bounded `reason` identifies version/limit |
| Invalid reference, missing capability, changed applicable pin | Input/plan: `invalid_reference`, `unknown_capability`, `plan_stale` | No substituted binding/native profile; recompilation is new admission |
| Current permission or finite budget denied | Policy: `permission_denied`, `budget_exceeded` | No handoff; no reinterpretation as resource wait |
| Unsupported model/control, unavailable native compute/capacity | Availability: `unsupported_model`, `capability_unavailable`, `native_compute_unavailable`, `resource_unavailable` | Only explicit bounded wait/retry policy; unsupported control rejects the command without altering Job state |
| Native execution, provider, or retained-state failure | Execution: `native_execution_failure`, `upstream_failure`, `state_unavailable` | Stop affected work; preserve state/cleanup ownership and known effects |
| Bad result/schema/encoding or output overflow | Contract: `invalid_result`, `result_too_large` | Reject before binding/injection; paid/write evidence remains |
| Job or Run elapsed deadline | Deadline: `job_timeout`, `run_timeout` | Failed intent and bounded cleanup; may also carry unknown external outcome |
| Response lost after possible provider handoff | Uncertainty: `outcome_unknown` | Reconciliation required; no blind redispatch or zero-cost assumption |
| Release/containment error or cleanup deadline | Containment: `cleanup_failed`, `cleanup_timeout` | Keep charged cleanup record; attach to original stop cause |
| Unexpected exception or broken invariant | Internal: `internal_error`, `invariant_violation` | Fail closed; safe message and bounded diagnostic reference |

`race_no_acceptable_result` is the Phase A group failure code, classified as
execution failure. Result access may return `job_cancelled`/`run_cancelled` only
for committed cancellation. Cancel with unresolved external outcome fails with
`outcome_unknown`; timeout keeps its timeout code and adds the uncertainty.
Cleanup failure never replaces the first committed stop cause. Retrying remains
a new bounded attempt under current authorization, not an error-handler loop.

## Closed wire versions and shared bounded JSON profile

Select exact version tokens: `flamoris.workflow/0.1`, `flamoris.submit/1`,
`flamoris.event/1`, `flamoris.error/1`, and internal semantic export
`flamoris.plan/1`. These are proposed first encodings, not backward-compatibility
claims for the Phase A illustrative `0.1-draft` examples. Writers emit only their
declared version. Executable input rejects every unrecognized version, key,
enum, tag or effect; there is no best-effort interpretation of future inputs.
Breaking execution semantics requires a new workflow/compiler revision.

Accept strict UTF-8 JSON objects, reject duplicate decoded property names,
invalid Unicode, trailing data, comments, NaN/infinity and non-JSON numeric
syntax. Reject a byte-order mark rather than choosing parser-dependent behavior.
The numeric value profile is finite binary64 within ±(2^53−1); count/duration
fields must be exactly representable nonnegative integers. Reject a nonzero
numeric token that underflows to zero, and reject out-of-range integer tokens
before conversion loses information. Higher precision and monetary values use
schema-declared decimal strings; no locale or implicit number/string coercion.
Identifiers and counters that may span uint64 serialize as canonical unsigned
decimal strings, `0` or a nonzero digit followed by at most 19 digits, with a
checked uint64 bound. Negative zero has the canonical numeric meaning zero.

| Input/record bound | Baseline hard maximum; deployment may lower it |
| --- | --- |
| Decoded request/IR bytes | 1 MiB each; ingress enforces byte cap before DOM allocation |
| JSON container nesting | 32; count during parsing before descending |
| Single decoded string / total scalar bytes | 256 KiB / 1 MiB per request |
| Static invocation/control nodes, including nested groups | 256 total |
| Explicit plus reference-derived edges | 1,024 after deduplication; repeated explicit edge is invalid |
| References / components per path | 4,096 / 32 |
| Identifier / object key bytes | 128 / 256 UTF-8 bytes |
| Group members / group nesting | 64 / 8, subject to total-node cap |
| Acceptance expression nodes / nesting | 64 / 8 |
| One error envelope / message / cause-code entries | 2 KiB / 256 UTF-8 bytes / 8 |
| One event envelope, including payload | 4 KiB; full groups reserve aggregate capacity before commit |

These are admission ceilings, not a promise every graph under them fits all
budgets. Dynamic fragments share remaining Run counts/bytes/deadlines; they
never receive a fresh 256-node allowance. Runtime-configured event reservation
uses the finite lifecycle cardinality formula in [Concurrency](CONCURRENCY.md).
Results obey the smaller registered output and Run bounds. Oversized external
results are stopped while receiving; no allocate-everything-then-check path.

nlohmann's SAX interface permits early termination [S1]; use it to enforce
nesting/counts, decoded-name uniqueness and schema-independent type checks while
constructing a bounded owned value tree. The byte cap remains necessary because
a parser may allocate a complete string token before its callback. Parser-native
diagnostics stay internal; return safe field IDs and offset only. Decompression,
if later supported, must cap decoded bytes and ratio before this boundary.

## Workflow profile: exact structural grammar

The following is schema notation, **not C++ or executable JSON Schema**.
Objects have exactly the listed required keys and explicitly optional keys;
`Map<T>` is an object of distinct bounded names; `List<T>` is a bounded array.
An identifier is ASCII `[A-Za-z_][A-Za-z0-9_.-]{0,127}`. Namespace resolution is
case-sensitive. Output/input field names can be other valid bounded Unicode
strings; paths are arrays, so dots never introduce implicit traversal.

| Shape | Required members | Optional members / meaning |
| --- | --- | --- |
| Workflow | `schema_version`, `workflow`, `inputs`, `nodes`, `edges`, `outputs`, `limits` | None; version is `flamoris.workflow/0.1` |
| Workflow metadata | `id` | `name`; both are display/provenance, not authority |
| `inputs` | `Map<ValueSchema>` | Declarations; concrete immutable values come from submission `input_values` |
| `nodes` | Nonempty `List<Node>` | Nested group members count toward the same total |
| `edges` | `List<Edge>` | Edge is exactly `{from: NodeId, to: NodeId}` in the current top-level DAG |
| `outputs` | Nonempty `Map<Binding>` | Compile must prove each referenced output reachable and schema-compatible |
| Invoke Node | `id`, `type`, `with` | `timeout_ms`, `retry`, `child_policy`; `type` resolves a registered capability, not an endpoint |
| Control Node | `id`, `type`, `with`, `control` | None; type is `control.await`, `control.join` or `control.race`; timeout belongs only to `control` |
| `with` | `Map<Binding>` | Invocation arguments, or local immutable inputs for a control group's members |
| Literal binding | Exactly `{literal: JsonValue}` | No interpolation; capability-schema service handles must enter through typed inputs instead |
| Reference binding | Exactly `{ref: Reference}` | Reference has `source`, `name`, `path`; source is `input` or `node`; path is a list of object-key strings or nonnegative integer array indices |
| Join control | `members`, `policy`, `timeout_ms` | members are a nonempty list of Node objects; policy `all_success` or `all_settled` |
| Await control | `members`, `timeout_ms` | Exactly one Node member; implicit `all_success` |
| Race control | `members`, `accept`, `loser_policy`, `timeout_ms` | Fixed nonempty Node list; loser policy exactly `cancel_unfinished` |
| Retry policy | `max_attempts`, `backoff_ms` | Count 1–8, finite nonnegative backoff; absent means one attempt; registry contract may prohibit retry |
| Child policy | Bounded logical registered policy-template identifier | Absent means explicitly empty dynamic-child envelope; resolution pins the complete template revision and intersects current caller/Run bounds |

The same ID is unique within its lexical scope. A compiler-generated structural
path disambiguates nested members without changing user IDs. Each control node
materializes one coordinating Workflow Job; its members become its owned child
Jobs. The root Workflow Job owns top-level node Jobs. Control-group members may
reference only their group's local inputs, not one another or outer nodes. A
top-level group's `with` may reference admitted inputs/preceding top-level
nodes. A nested group's `with` may reference only the immediately enclosing
group's local inputs; values needed further inside must be passed explicitly
through each enclosing `with`. There is no implicit outer-scope lookup. This
makes the closed child set and ownership explicit. Sequential work
and cross-node dependencies use the ordinary top-level DAG; a dependent group
is itself an ordinary node. No detached Jobs or cross-group child ownership.

Reference-derived dependencies join explicit edges before cycle/effect checks.
For node outputs, `name` identifies the node and `path` traverses its declared
output value. An input reference names the declared input. Reject unavailable
fields, invalid array indices and non-predecessor dependencies; no environment,
network, filesystem, credential or arbitrary expression lookup exists.
The `${...}` strings in Phase A examples remain illustrations, not a second
parser: in this profile a string inside `literal` is always literal text.

Join output schemas are fixed by compiled member schemas. `all_success` and
await return `{results: [...]}` in member declaration order (await has one
entry). `all_settled` returns `{outcomes: [...]}`, each exactly
`{status: "succeeded", value: V}` or
`{status: "failed"|"cancelled", error: ErrorEnvelope}`. Race returns
`{participant: MemberId, value: V}` for the committed winner. The compiler
derives the appropriate discriminated union of member value schemas; callers
cannot treat an all-settled failure as an ordinary successful member value.

Race `accept` is one total bounded expression applied to candidate output:
`{op:"always"}`, `{op:"exists",path:P}`, `{op:"eq",path:P,value:Scalar}`,
`{op:"all"|"any",args:[Expr,...]}`, or `{op:"not",arg:Expr}`.
`Scalar` is null/boolean/number/string within the JSON profile. Missing paths
make `exists`/`eq` false; incompatible scalar types compare unequal; all/any
require a nonempty list. No coercion, regex, recursion, code or I/O. The compiler
validates candidate-schema paths; optional fields are allowed but unknown fields
are not. Candidate failure never evaluates as a successful winner. Batch ties
preserve member order; normal ties use Run commit order.

`ValueSchema` is a closed bounded JSON-value subset: `type` is null, boolean,
integer, number, string, array or object. Numeric types require `minimum` and
`maximum`; string requires `max_bytes`; array requires `items` and `max_items`;
object requires `properties`, `required` and `additional_properties:false`.
No recursive references, regex, remote `$ref` or dynamic schema import. A
registered capability can attach a logical service-handle type to a schema;
its owner/scope validator is mandatory at binding and use. This is registered
metadata, never a caller-authored permission field. Rich external schemas must
be mapped into this supported bounded subset by their trusted registration.

`limits` is a closed map of finite nonnegative integers:
`timeout_ms`, `max_parallelism`, `max_jobs`, `max_attempts`,
`max_dynamic_proposals`, `max_child_depth`, `max_output_bytes`,
`max_control_steps`, `max_suspensions`, `max_control_commands`,
`max_resource_operations`, `max_event_bytes`, and `max_trace_bytes`.
`timeout_ms`, `max_jobs`, `max_attempts`, `max_control_steps`,
`max_parallelism` and `max_output_bytes` must be positive. Omitted entries are
filled from an explicitly versioned deployment profile; zero forbids the
associated optional behavior. All effective values and resource/cost envelope
ceilings enter the compiled plan. Caller values may tighten, never enlarge,
deployment or capability bounds. External paid effects derive from trusted pinned capability policy and finite
local Run allowances; workflow JSON cannot declare itself free. Strict durable
monetary ceilings are an optional future profile. Integer overflow rejects before expansion.

All timeout durations are positive integer milliseconds. The Run's absolute
deadline is admission's monotonic timestamp plus its effective `timeout_ms`.
An invocation Job's deadline is the minimum of its ancestor deadline and its
creation timestamp plus node timeout (absent means inherit the ancestor).
A control group's deadline is the minimum of its ancestor deadline and its
coordinating Job's creation timestamp plus `control.timeout_ms`; there is no
second outer Control Node timeout. Member deadlines are clamped to that group
deadline. Queue, loading, wait, pause and retry consume these same deadlines;
readiness, dispatch, resume and a changed wall clock cannot reset them.

## Submission normalization and compiler stages

The transport-neutral request is an exact discriminated union, with
`schema_version:"flamoris.submit/1"` and optional bounded opaque
`idempotency_key` in both branches. The `kind:"workflow"` branch requires
`workflow` (the profile above) and `input_values`. The `kind:"inference"`
branch requires `inference` and forbids `workflow` and `input_values`; its
`inference` object requires `type`, `with`, `limits`, with optional `child_policy`.
The workflow branch forbids `inference`. There are no ignored members.
Authenticated caller context is supplied outside this object. Direct inference
`with` contains bounded concrete literal input values, validated against the
registered inference contract. The compiler normalizes it into a single-root inference
plan, with the same pinned model/processor/tokenizer/execution-profile/compute/schema/effects/resources and an empty
child envelope by default. It never invents a wrapper that changes Job identity
or bypasses compilation/admission. It lifts those concrete fields into immutable
Run input bindings and typed plan input slots; service handles never become
plan constants merely because the caller used the direct request form. Both
inputs use the same compiler pipeline.

`idempotency_key` is optional by absence only. When present it must be a
nonempty opaque UTF-8 string of at most 256 bytes; `null`, empty strings,
non-string values and oversized keys reject before admission. Do not trim,
case-fold or replace a missing key with a shared sentinel.
An absent key disables submission deduplication: each accepted request gets a
fresh Run identity even when its content/digest equals another request, whether
sequential or concurrent. The digest can still support internal audit/content
binding, but it never becomes a SubmissionIndex lookup key. Explicit keys use
the atomic A27 claim contract in [Concurrency](CONCURRENCY.md).

1. Bound and parse; reject duplicate keys/unknown version/keys and invalid values.
2. Resolve declared types and all bindings against a captured immutable registry
   snapshot; expand no user-defined macros and acquire no external resources.
3. Resolve registered capability/adapter/model and child-policy pins, derive
   schemas/effects/finite maximum obligations, fill explicit profile defaults.
4. Normalize reference dependencies, reject cycles, unreachable outputs and
   unproven conflicting effect order. Prove group ownership and paid-race
   aggregate worst-case liabilities. Apply static scope analysis, not grants.
5. Assign deterministic structural indices by stable topological order, with
   ASCII node ID as the ready-set tie-breaker. Preserve member order and any
   list whose order changes outcomes; sort only semantically unordered sets.
6. Emit immutable typed plan and canonical semantic export; compute fingerprint.
   Admission then performs current authentication, authorization and reservations.

No mutable registry pointer escapes compilation. The control executor verifies
applicable pins on the returned plan before admission. An unrelated registry
change does not stale it. Input value size/scope checks and dispatch/retry/resume
checks remain necessary even if a cached plan has the same fingerprint.

## Canonical bytes and identities

Choose RFC 8785 JSON Canonicalization Scheme for the **normalized semantic
export**, then SHA-256 over `UTF8("flamoris.plan/1\n") || JCS(export)`.
Use UTF-16 code-unit key ordering, canonical primitive spelling and UTF-8 output;
preserve Unicode strings without normalization. JCS rejects duplicate names
and invalid Unicode and constrains numbers to binary64 [S2]. Do not substitute
ordinary map iteration or a library's default JSON dump. The selected
implementation must pass RFC number/string/order vectors plus our stricter
numeric bounds before it creates a plan ID. SHA-256 is provided by the reviewed
crypto dependency, not new project cryptographic code.

The export includes schema/compiler/canonicalization revisions, all normalized
steps and bindings, literal values, input/output schemas, exact applicable
capability/adapter/model/child-policy pins, effect/order rules, finite effective
limits, and scheduling/resource-policy revisions that alter semantics. Exclude
workflow display name/id, canvas metadata (which this wire profile does not
accept), credentials, endpoint configuration, current availability, live object
handles, Runtime IDs, permission decisions, absolute timestamps and state.
Service-handle bindings remain typed input slots; concrete handles are Run
inputs and are not exported as plan constants. Immutable semantic input values
that determine compile-time branch selection must be represented explicitly as
plan literals, or the compiler must leave that selection to runtime binding.

Submission deduplication uses a **separate** SHA-256 domain
`flamoris.submission/1\n` over JCS of the structurally validated request before
any mutable registry/profile resolution. Include all accepted fields, including
version, request kind, workflow/direct input, concrete workflow `input_values`,
caller-written limits and selected policy-template names; remove only the
`idempotency_key` and the workflow display/provenance `workflow.workflow` object
(`id`/`name`). Transport metadata and authenticated context are not JSON input
fields. Do not insert current deployment defaults, capability metadata, compiler
indices or timestamps into this digest. An omitted default and an explicitly
written value are distinct request content; that conservative distinction is
stable across configuration changes. Structural JSON normalization applies JCS
only, including canonical numeric spellings and object order, and preserves
array order. The claim key additionally scopes local subject and request kind.
Its digest does not include transient registry/policy state, so a
duplicate returns its original admission decision/Run rather than a new Run
after registry change. Different input/digest conflicts even while pending.
Typed service-handle identity/scope participates in this private request digest;
never emit raw input digests or hidden inputs into public traces by default.

Provider-operation idempotency and confirmation digests have distinct domains
and include their required concrete principal/target/input binding. They are
not inferred from plan or submission identity. Hash collision resistance does
not authorize a request or establish that an external side effect occurred.

## Event, error and plan projections

`EventEnvelope` serializes the Phase A fields exactly: `schema_version`, `kind`,
`runtime_instance_id`, `run_id`, `seq`, `event_id`, `transition_id`,
`group_index`, `group_size`, `monotonic_offset`, `wall_time`, `payload`; optional
`job_id`, `attempt_id`, `causation_id`, `command_id`. Sequences/offsets use
unsigned decimal strings; `monotonic_offset` is integer nanoseconds since the
Runtime incarnation's injected steady-clock origin (checked conversion, no
cross-incarnation comparison). Wall time is informational UTC RFC 3339 text or
null when unavailable. Event identity is the runtime/Run/sequence tuple, exposed
opaquely; sequence is allocated only at commit. Group indices are zero-based.
The whole group is contiguous, bounded and committed atomically with state.

Payloads are closed discriminated shapes for the Phase A event families, using
typed identities, old/new states, reason/error/result references, operation
epochs and applicable ledger revisions. Large collections are pre-existing
bounded authorized references, never an oversized 4 KiB envelope. Reserve
event/group capacity before accepting the operation that needs it; observation
subscriber backpressure cannot reject a state commit. Telemetry may be sampled
with explicit gaps, never silently drop required lifecycle/reconciliation facts.

An observation page supplies `events`, complete-group boundaries or explicit
incomplete-group metadata, `earliest_retained_seq`, committed `watermark`, and
stream state (`open`, `closed`, `expired`). Snapshot plus later groups use that
watermark. Post-terminal reconciliation uses the same stream and higher seq,
but cannot change frozen terminal result or accept new useful work. On closure,
ledger reconciliation continues without reopening the stream. Inspection-only
replay validates versions/groups and uses a separate projection with no ports
that can invoke effects; unknown kinds/versions, missing groups and redaction
produce an explicit incomplete projection, not guessed lifecycle facts.

`ErrorEnvelope` required members: `schema_version`, `code`, `category`,
`message`, `stage`, `external_outcome`, `retry_disposition`, `cause_codes`.
Optional: `reason`, `run_id`, `job_id`, `attempt_id`, `diagnostic_ref`.
`external_outcome` is `not_applicable`, `not_dispatched`, `confirmed_success`,
`confirmed_failure`, or `unknown`; `retry_disposition` is `prohibited`,
`policy_eligible`, or `reconciliation_required`. Missing provenance fields are
omitted, not fabricated before admission. `stage` is validation/admission/
dispatch/execution/result_validation/cleanup. A failed provider outcome may
still carry partial-effect evidence in the correlated bounded operation record.

Unknown executable versions fail closed. An observer may preserve an unknown
event/error as bounded opaque evidence and mark projection incomplete; it must
not map it to success or execute it. Plan export is inspection data only. No
import API reconstructs Job, Continuation, native pointer, authorization or
lease state from JSON, even when the version matches.

## Redaction and tests

Construct error/event payloads from allowlisted typed fields **before** placing
them in retained buffers, logs or subscribers. Drop raw upstream messages/bodies,
secrets, host paths, endpoints and credential material, including nested causes.
Sensitive token/input/output traces require explicit current trace authorization
and finite bounds; ordinary lifecycle records use scoped references. Access
control runs again for retained history, export and replay. Sanitization is not
just a presentation filter on the final HTTP response.

Required deterministic cases include duplicate decoded keys, invalid UTF-8 and
surrogates, huge numeric lexemes, exact bound/one-over-bound limits, deep arrays,
unknown tags/version, UTF-16 versus UTF-8 sort fixtures, equivalent JSON spelling,
member-order significance, stable topological order, changed pins/profile,
same-key changed inputs, empty/unknown effects, typed all-settled failures,
oversized paid output with retained liability, grouped event pagination,
expired-stream reconciliation, nested error redaction and allocation failure
during preparation before the no-throw commit. Preparation failure must expose
no mutation or partial group. Separate invariant fault injection at commit
verifies fail-stop containment and no false success, not an invented rollback
mechanism. No GPU or network is necessary.

## Primary serialization evidence

Retrieved 2026-09-27. The bounded workflow grammar and caps above are FLAMORIS
design decisions, not claims that upstream libraries enforce them.

- S1: [nlohmann JSON SAX interface](https://json.nlohmann.me/features/parsing/sax_interface/).
- S2: [RFC 8785, June 2020, sections 3.1–3.2 and Appendix B](https://datatracker.ietf.org/doc/html/rfc8785).
