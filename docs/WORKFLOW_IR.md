# Workflow IR

**Product boundary:** native inference uses FLAMORIS-owned execution state. External AI runtimes/providers are registered Workflow capabilities with effect and authorization checks; they cannot substitute for the native Inference Machine. Baseline is single-user.

## Status

**Phase A semantic design. No schema is implemented; wire representation is deferred to Phase B.**

[Execution Model](EXECUTION_MODEL.md) and [State Machines](STATE_MACHINES.md) define execution semantics. [Design Phases](DESIGN_PHASES.md) defines the review gates.

The Workflow IR describes dependencies, data flow, and bounded control inside FLAMORIS AI Runtime.

It is not the Runtime itself and it is not the scheduler state.

## Relationship to inference and jobs

Keep these concepts separate:

```text
Workflow IR
   │ declarative dependencies/control
   ▼
Validator
   ▼
Execution Plan Compiler
   ▼
Runtime Execution Plan
   │ normalized execution contract, never an authorization grant
   ▼
Jobs
   │ scheduler-visible; may own Continuations while suspended
   ▼
Capabilities / Inference
```

Inference may itself be represented by a stateful `InferenceJob`.

A running inference may yield, dispatch child jobs, wait for them, receive bounded results, and resume where the backend supports state preservation.

The Workflow IR therefore must be able to express useful coordination without becoming a general programming language.

## Goals

The Workflow IR should be:

- declarative;
- JSON-serializable;
- schema-versioned;
- deterministic to validate;
- independent of GUI layout;
- portable across MCP, API, CLI, Studio, tests, and AI planners;
- able to compose inference, algorithms, Vision/audio, future Vem, external AI/API, MCP, Generation, and other registered capabilities;
- explicit about dependencies and side effects;
- compatible with logical parallelism;
- bounded enough to reject unsafe or unreasonably expensive graphs before execution.

## Non-goals

The IR should not initially be:

- a general programming language;
- arbitrary code;
- raw scheduler state;
- a dump of KV cache/model tensors;
- a visual editor save format;
- a provider configuration dump;
- a credential envelope;
- durable Agent memory;
- durable Generation asset state.

## Draft top-level shape

Conceptually:

```json
{
  "schema_version": "0.1-draft",
  "workflow": {
    "id": "example",
    "name": "Example workflow"
  },
  "inputs": {},
  "nodes": [],
  "edges": [],
  "outputs": {},
  "limits": {}
}
```

Exact names remain draft.

## Compilation boundary

Workflow IR is never scheduler state and should not be executed directly.

The Runtime first validates the IR, resolves registered capabilities and bindings, applies caller/runtime limits, derives resource/effect information, and compiles an **Execution Plan**. Direct inference submission is normalized into a minimal single-root inference plan under the same compiler and admission contract, including pinned model/backend, effects, bounds and an explicit dynamic-child envelope; see [Execution Model](EXECUTION_MODEL.md). It does not bypass plan compilation merely because the caller did not supply Workflow IR.

Conceptually:

```text
Workflow IR
  ↓
schema / graph / reference validation
  ↓
capability resolution
  ↓
static permission/effect/budget requirement analysis
  ↓
Execution Plan Compiler
  ↓
Execution Plan
```

The plan may contain:

- resolved capability/version references;
- normalized steps;
- dependencies;
- input/output bindings;
- resource requirements and affinity hints;
- effect sets;
- side-effect boundaries;
- runtime limits;
- statically known potential suspension sites and continuation policy.

The plan is Runtime-owned and immutable. It pins IR/compiler semantics, capability/schema/effect/control contracts and adapter revisions, with a canonical fingerprint. Same inputs, capability snapshot, compiler version and static limits produce the same normalized semantics or rejection. It may use internal identifiers outside the portable IR schema.

Each dispatch verifies applicable pins; contract changes yield `plan_stale`, never silent substitution. Availability changes do not rewrite the plan. Detailed version/policy handling follows [Authorization Model](AUTHORIZATION_MODEL.md).

The plan is not an authorization token. Current pins/availability, caller authorization, concrete input scope, budgets and policy are checked at admission and every dispatch, retry and resume, with effect-specific checks before adapter handoff. Cached/reused plans must not preserve stale permission.

Concrete Continuation instances are never compiled into the plan. They are runtime-owned state created when a scheduler-visible Job actually yields, including inference yields whose exact occurrence is only known during execution.

## Nodes and jobs

A node describes a logical operation.

A job is the runtime execution unit created from that logical operation.

A node should minimally identify:

- `id`;
- `type`;
- configuration/input bindings.

Example:

```json
{
  "id": "describe",
  "type": "vision.describe",
  "with": {
    "image": "${inputs.image}"
  }
}
```

Node IDs must be unique.

Unknown node types are validation errors, not dynamic imports.

A node type's registered capability metadata determines:

- input/output schema;
- effect set;
- idempotency;
- cancellability;
- pause/resume support if meaningful;
- resource class;
- timeout/resource bounds;
- whether it may create child jobs.

## Capability classes

Conceptual families may include:

- `model.*` or inference-specific operations where intentionally exposed;
- `algorithm.*`;
- `vision.*`;
- `speech.*`;
- `generation.*`;
- `vem.*` after Vem has a stable callable contract;
- `external_ai.*`;
- `mcp.*`;
- explicit FLAMORIS product/service capabilities;
- `control.*` and `data.*` runtime primitives.

These names are not frozen.

## Data flow

Structured outputs should flow directly when schemas are compatible.

Examples:

```text
Vision result -> algorithm
Vision result -> inference context
speech recognition -> inference
inference -> TTS
algorithm result -> MCP
Vem result -> Generation
race winner -> inference resume
```

Large media should use bounded handles/references or approved streaming/materialization paths rather than repeated base64 embedding.

## Edges and readiness

Edges represent dependencies.

Independent ready nodes may execute concurrently.

```text
        ┌─ node A ─┐
input ──┤          ├─ node C
        └─ node B ─┘
```

The Workflow IR describes logical readiness.

The scheduler decides whether ready jobs execute physically at the same time.

Validation should reject:

- missing references;
- invalid/duplicate edges;
- unsupported cycles;
- impossible dependency ordering;
- fan-out beyond limits.

The baseline accepts bounded DAGs only. Validate node/edge/reference counts and nesting before expansion, require compatible binding schemas, and reject references to unavailable/non-predecessor outputs. Compilation includes reference-derived dependencies in cycle detection and readiness; a binding cannot bypass dependency order by omitting an explicit edge.

Every declared output must be type-compatible and reachable from admitted inputs or reachable producer nodes. Empty executable graphs are rejected in the baseline; identity transforms must be explicit registered operations. Control predicates and reference evaluation are bounded, total and free of hidden I/O. Exact wire grammar is a Phase B deliverable.

For effects that can conflict, the compiler requires an explicit dependency/order or a registered contract proving disjoint scoped operations. The baseline rejects unordered potentially conflicting writes or read/write pairs when disjointness cannot be proven. Resource serialization alone does not choose their semantic order.

## Await, join, and race

The precise semantics are defined in [Execution Model](EXECUTION_MODEL.md):

| Operation | Baseline behavior |
| --- | --- |
| `await` | One-child `all_success`, with bounded deadline and explicit failure propagation |
| `join.all_success` | Require every named child to succeed; first required failure cancels unfinished siblings |
| `join.all_settled` | Wait for terminal outcomes and return typed success/failure values in declared order |
| `race` | Fixed participants, bounded deterministic acceptance, first accepted success in Run commit order |

Participants must be a nonempty, unique, closed child set. Unknown policies, unbounded acceptance and ownership violations are invalid. Acceptance cannot invoke capabilities or I/O. Group deadlines cannot extend the Run.

Initial race loser policy is `cancel_unfinished`; loser cleanup stays owned even when the parent resumes with a winner. `write`/`destructive` races are excluded; paid races require aggregate worst-case cost reservation across all participants and attempts. Losing or cancelling is not rollback. Provisional result replacement and continuing losers for caching are deferred.

Failure recovery is a declared typed binding, never an implicit success value or a new branch invented by model output. Exact JSON encoding is designed and reviewed in Phase B **before** executor implementation.

## Parallelism and resource policy

Workflow authors may express dependencies and optionally tighter concurrency limits.

They must not force unsafe physical parallelism.

Example:

```json
{
  "limits": {
    "max_parallelism": 4,
    "timeout_seconds": 120
  }
}
```

Effective limits are bounded by server/runtime policy.

GPU-heavy jobs may be logically parallel but physically serialized due to VRAM, model residency, or device constraints.

## Inference handoff

The IR should support a model inference flow that temporarily yields to other jobs and resumes.

Conceptually:

```text
InferenceJob
   ↓
needs external/local work
   ↓
child jobs
   ├─ Vision
   ├─ algorithm
   └─ MCP
   ↓
join / race
   ↓
inject selected bounded result
   ↓
InferenceJob resume
```

The IR should not contain raw KV cache or backend pointers.

Those belong to runtime execution state.

## Continuations

Workflow IR may describe control that can yield or wait, but it must not serialize raw Continuation objects, backend pointers, KV cache, GPU leases, or other live Runtime state.

The compiler may identify **potential suspension sites** and continuation policy from validated workflow/capability semantics, such as `await` or `join`. It does not pre-create concrete Continuations.

The Runtime creates a Continuation only when a Job actually suspends. Runtime-defined inference yields may therefore create Continuations at control points whose concrete occurrence could not be known during compilation.

Every Continuation is owned by exactly one waiting/paused Job and has no independent scheduler identity. On wake it is consumed atomically into that same queued Job’s pending resume payload. State lifetime and resource accounting survive this transfer; see [State Machines](STATE_MACHINES.md).

A Continuation may hold:

- resume point;
- waiting condition;
- bounded result bindings;
- backend/state reference where supported;
- resource requirements and affinity hints;
- deadline/cancellation linkage.

This lets inference and workflow resume through a common Runtime mechanism without turning Workflow IR into a process snapshot format.

## References

References should remain small and deterministic.

Candidate forms:

- `${inputs.name}`;
- `${nodes.node_id.output}`;
- `${nodes.node_id.output.field}`.

Do not embed a full scripting language in references. The baseline permits only finite validated input/node-output paths. Missing fields, incompatible types, cycles and references beyond declared producer schemas are errors. There is no environment-variable, credential, filesystem or network interpolation.

Large service-owned handles are validated for owner/scope and size at binding and use. Materializing a remote object or reading mutable state is an explicit effectful capability, not hidden reference evaluation.

## Outputs

Workflow outputs select already-produced values.

Large assets should normally remain owned by the capability/service that produced them and be returned as explicit references.

Output size is bounded.

## Effects and side-effect analysis

Effect classification belongs to registered capability metadata, not to claims in workflow JSON.

Effects are a composable set, but not every combination is valid. Initial conceptual values include:

- `pure` - only declared immutable inputs/outputs, with no ambient read, external boundary, mutation, destructive action, or policy-relevant cost;
- `read` - reads ambient or mutable state beyond declared immutable inputs;
- `write` - mutates persistent or externally observable state;
- `external` - crosses an external process/service/network authority boundary;
- `destructive` - destructive mutation and therefore requires `write`;
- `paid` - may consume monetary, billed-token, quota, or similar policy-relevant cost.

Initial validation rules:

- effect sets are non-empty and contain only known values;
- `pure` is exclusive with every other effect;
- `read` and `write` may coexist;
- `destructive` without `write` is invalid;
- `external` and `paid` are orthogonal attributes and may combine with non-pure read/write/destructive effects;
- unknown or contradictory sets fail closed.

For example, local deterministic resize may be `{ pure }`, remote paid inference may be `{ external, paid }`, issue creation may be `{ external, write }`, and deletion may be `{ write, destructive }`.

The validator/compiler derives a conservative effect summary before execution, including possible dynamic children, optional branches and race losers, and marks side-effect boundaries. Aggregation validates each set first, removes `pure` while combining observable effects, and returns `{pure}` only for a nonempty all-pure composition. See [Authorization Model](AUTHORIZATION_MODEL.md) for the normative algebra and current authorization rules.

This allows authorization, confirmation, budget checks, retry policy, race policy, and event provenance to reason about effects without trusting workflow-authored claims.

## Interrupts and workflow changes

Runtime interrupt is not the same as arbitrary graph mutation.

An interrupt may request:

- pause;
- stop/cancel;
- bounded input injection;
- change of an unexecuted branch.

Baseline plans are immutable. General graph patching, changed unexecuted branches and rewind are deferred; mention above describes future control intent, not a baseline mutation API. Later adaptive patching requires explicit revisioned validation and must not rewrite completed effects.

## Dynamic inference child envelope

An inference capability may request runtime-generated child work only inside a compiled envelope with pinned allowed capabilities, schemas, scopes/effects, depth/count/fan-out/attempt limits, cost/resources/output bounds, suspension policy and parent failure behavior. The Runtime validates and compiles each bounded child fragment before registration and normal dispatch checks. The parent and every fragment share cumulative Run limits and deadlines.

Unknown or out-of-envelope requests fail closed or use an already declared typed error binding. No dynamic registration, endpoint, credential, detached child or limit reset is allowed. Repeated invalid proposals consume a finite budget. See [Execution Model](EXECUTION_MODEL.md).

## Capability metadata

Conceptual metadata:

```json
{
  "type": "mcp.github.issue.get",
  "version": "1",
  "input_schema": {},
  "output_schema": {},
  "effects": ["external", "read"],
  "idempotent": true,
  "cancellable": true,
  "pausable": false,
  "resource_class": "network"
}
```

This is illustrative only.

## Secrets and endpoints

Secrets must not be embedded in workflow JSON.

Portable workflows should reference registered logical capabilities, not arbitrary endpoints or raw credentials.

## Versioning

Breaking execution-semantic changes require a schema-version change.

Unsupported future schemas should be rejected deterministically rather than interpreted on a best-effort basis.

## Design-to-implementation scope

Phase A specifies the bounded semantics; Phase B defines exact schemas, compiler stages, C++ contracts and deterministic test seams; Phase C implements reviewed slices. The baseline covers DAG validation, explicit references, compiled plans, Job creation, basic join/race, effect/authority checks, Continuations and bounded structured observation.

In-memory inspection-only replay belongs to baseline acceptance (A32). Durable event persistence/recovery, advanced residency optimization, adaptive graph mutation and speculative replacement remain later extensions. There are no dynamic loops, arbitrary code or durable distributed scheduling in the baseline. See [Design Phases](DESIGN_PHASES.md) and [Design Acceptance](DESIGN_ACCEPTANCE.md).
