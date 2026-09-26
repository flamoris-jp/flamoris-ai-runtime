# Workflow IR

## Status

**Draft design. No schema is implemented or frozen yet.**

The Workflow IR describes dependencies, data flow, and bounded control inside FLAMORIS AI Runtime.

It is not the Runtime itself and it is not the scheduler state.

## Relationship to inference and jobs

Keep these concepts separate:

```text
Workflow IR
   │ describes dependencies/control
   ▼
Runtime execution plan
   │
   ▼
Jobs
   │ scheduled according to resources
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
- side effects;
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

The first implementation should prefer DAG execution.

## Await

A node/run may wait for one required job/result.

Await semantics should be explicit around:

- timeout;
- cancellation;
- failure;
- result availability.

## Join

`join` waits for a defined set of branches/jobs.

Conceptual policies may include:

- require all;
- require selected named children;
- collect successful children when explicitly allowed.

The initial policy set should remain small and deterministic.

## Race

`race` selects the first candidate result meeting an explicit acceptance condition.

Conceptually:

```text
            ┌─ local model ───────┐
request ────┼─ remote specialist ─┼─ race -> selected result
            └─ cached path ───────┘
```

Race must define:

- participant set;
- winner condition;
- behavior when a participant fails;
- overall timeout;
- loser policy.

Possible loser policies may include:

- cancel unfinished losers;
- allow losers to finish for cache/provenance;
- retain already completed results.

Cancellation must never be described as rollback of completed side effects.

The exact JSON representation of race is intentionally deferred until the executor/job model is implemented.

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

## References

References should remain small and deterministic.

Candidate forms:

- `${inputs.name}`;
- `${nodes.node_id.output}`;
- `${nodes.node_id.output.field}`.

Do not embed a full scripting language in references.

## Outputs

Workflow outputs select already-produced values.

Large assets should normally remain owned by the capability/service that produced them and be returned as explicit references.

Output size is bounded.

## Side effects

Side-effect classification belongs to registered capability metadata, not to claims in workflow JSON.

Examples include:

- sending a message;
- changing product state;
- creating/publishing an asset;
- starting/stopping a service;
- calling a paid remote API.

The validator/compiler should be able to identify side-effecting jobs before execution where possible.

## Interrupts and workflow changes

Runtime interrupt is not the same as arbitrary graph mutation.

An interrupt may request:

- pause;
- stop/cancel;
- bounded input injection;
- change of an unexecuted branch.

Later adaptive workflow patching should use explicit revisioned patch operations and must not silently rewrite completed side effects.

## Capability metadata

Conceptual metadata:

```json
{
  "type": "vision.describe",
  "version": "1",
  "input_schema": {},
  "output_schema": {},
  "side_effects": false,
  "idempotent": true,
  "cancellable": true,
  "pausable": false,
  "resource_class": "gpu"
}
```

This is illustrative only.

## Secrets and endpoints

Secrets must not be embedded in workflow JSON.

Portable workflows should reference registered logical capabilities, not arbitrary endpoints or raw credentials.

## Versioning

Breaking execution-semantic changes require a schema-version change.

Unsupported future schemas should be rejected deterministically rather than interpreted on a best-effort basis.

## Initial implementation scope

The first implementation should be intentionally small:

- DAG validation;
- explicit references;
- bounded inputs/outputs;
- deterministic pure nodes;
- job creation from nodes;
- simple scheduler;
- independent-node parallel readiness;
- basic join;
- basic race with explicit loser cancellation policy;
- event emission;
- no dynamic loops;
- no arbitrary code;
- no durable distributed scheduler.

More adaptive behavior should follow only after the job/inference lifecycle is proven.
