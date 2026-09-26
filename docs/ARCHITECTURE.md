# Architecture

## Purpose

FLAMORIS AI Runtime is intended to be a general processing layer underneath external AI callers and to execute the declarative workflow graphs they author.

Its job is not to "be the AI". The planning intelligence remains outside the Runtime. ChatGPT, `flamoris-ai-agent`, Studio AI, or another Agent can decide what processing is needed and submit a workflow.

The Runtime provides a small, explicit, testable execution substrate where that caller can compose available capabilities without receiving ambient authority over the host.

The conceptual split is:

```text
ChatGPT / Agent / external AI
  │
  │ intent
  ▼
planner in the caller
  │
  │ workflow IR
  ▼
FLAMORIS AI Runtime
  │
  ▼
validator
  ↓
compiler / planner
  ↓
bounded executor
  ↓
registered capabilities
  ↓
results + provenance
  │
  ▼
caller evaluates the result and decides the next step
```

## Caller intelligence stays outside

A persistent Agent is a natural caller, but it is not part of the Runtime itself.

```text
FLAMORIS AI Agent
  ├─ identity
  ├─ conversation
  ├─ memory
  ├─ goals
  └─ policy
       │
       │ workflow IR
       ▼
FLAMORIS AI Runtime
  └─ execution state only
```

This separation allows the same Runtime to serve ChatGPT, the FLAMORIS Agent, Studio AI, and other clients.

It also means that adaptation does not require the Runtime itself to become an Agent. A caller can inspect a result and submit a new workflow. Later graph patching may optimize some cases, but the basic architecture already supports adaptive behavior outside the Runtime.

## Why a separate runtime?

FLAMORIS already has domain authorities:

- AI Agent for durable Agent state;
- Intelligence MCP for bounded intelligence execution;
- Generation MCP for generation workflows/jobs/assets;
- GPU Node Manager for runtime lifecycle;
- products for their own document/project state.

What is missing is a generic **execution graph layer** that can compose those capabilities without becoming their owner.

The runtime exists to answer:

> Given this declarative graph and these explicitly available capabilities, can it be executed safely and predictably?

## Workflow IR versus domain workflow

This distinction is important.

### Runtime workflow IR

Owned here.

It describes an execution graph:

- node identities;
- node types;
- dependencies;
- inputs and outputs;
- execution options;
- bounded control flow.

Its lifetime may be only one run.

### Domain workflow

Owned by the domain service.

For example, Generation MCP may own a durable media-generation workflow, job state, provider selection, and generated assets.

The runtime may contain a node that asks Generation MCP to perform domain work, but that does not transfer Generation authority into this repository.

```text
AI Runtime run
   │
   └─ node: generation.submit
            │
            ▼
      Generation MCP
      owns its job/assets
```

The same principle applies to Agent state, Intelligence execution, GPU lifecycle, and product state.

## Headless first

The execution contract must be valid without a GUI.

A future visual editor may attach presentation metadata such as:

- node coordinates;
- colors;
- collapsed groups;
- notes;
- viewport state.

That metadata must not determine execution semantics.

The portable IR should remain usable from MCP, CLI, tests, Studio, or another AI.

## Runtime layers

The intended internal layering is:

```text
MCP / API / CLI adapters
          │
          ▼
     Admission layer
          │
          ├─ authentication context
          ├─ schema size/version checks
          └─ request limits
          │
          ▼
       Validator
          │
          ├─ graph checks
          ├─ node checks
          ├─ reference checks
          ├─ capability checks
          ├─ permission checks
          └─ budget checks
          │
          ▼
    Compiler / Plan
          │
          ├─ dependency ordering
          ├─ execution groups
          └─ explicit side-effect boundaries
          │
          ▼
       Executor
          │
          ├─ cancellation
          ├─ timeout
          ├─ concurrency limits
          ├─ result limits
          └─ structured events
          │
          ▼
 Capability adapters / handlers
```

The execution core should not know whether the workflow arrived over MCP, CLI, or tests.

## Processing examples

The intended layer is broad enough for ordinary multimodal AI processing.

For example:

```text
image
  ↓
Vision
  ↓
algorithmic feature extraction
  ↓
external reasoning AI
  ↓
TTS
  ↓
audio response
```

Or:

```text
media input
  ↓
algorithm
  ↓
Vem
  ↓
external MCP capability
  ↓
result
```

The first graph looks like a normal AI application. The difference is that the caller AI may decide to construct that graph dynamically instead of a developer hard-wiring the pipeline in advance.

## Capability classes

A single workflow may intentionally mix different execution styles:

```text
algorithmic processing ─┐
future Vem capability ──┤
external AI / API ──────┼─► one validated workflow graph
external MCP tool ──────┤
FLAMORIS service ───────┘
```

This mixing is one of the reasons to keep the workflow IR generic.

The architecture goal is **open-ended composition, bounded execution**. The Runtime should not impose arbitrary product categories on registered capabilities. If two capabilities have valid contracts and the caller is allowed to use both, the workflow model should generally permit their composition. Execution policy still constrains permissions, side effects, credentials, network/filesystem access, cost/resource budgets, and runtime limits.

The runtime should see all of them through one explicit capability model while preserving their different security and ownership properties:

- pure/local algorithms may be deterministic and side-effect free;
- Vem remains a future adapter boundary until its callable contract is stable;
- external AI/API calls may incur cost, latency, rate limits, and provider-specific failure;
- external MCP capabilities may perform arbitrary domain actions and therefore require explicit connection/tool authorization;
- FLAMORIS services retain their own domain authority.

## MCP in two directions

MCP can appear on both sides of the runtime, but these are different roles.

```text
AI / Agent
   │
   │ MCP control plane
   ▼
AI Runtime
   │
   │ registered workflow capability
   ▼
external MCP server/tool
```

The inbound Runtime MCP surface controls workflow admission and observation.

An outbound MCP capability is simply one registered executable capability inside a workflow. Its connection details, tool schema, authorization, and limits belong to runtime/deployment configuration, not to arbitrary workflow JSON.

## Capability registry

The runtime should expose a machine-readable capability registry.

A capability entry should eventually describe enough information for an AI planner to compose valid graphs without guessing:

- stable capability identifier;
- version;
- input schema;
- output schema;
- whether it has side effects;
- whether it is idempotent;
- permission requirements;
- resource class or budget hints;
- cancellation support;
- availability status.

The registry is not permission by itself.

A capability may exist but remain unavailable to a caller.

## Node families

Initial node families should stay small.

### Pure runtime nodes

Candidates:

- `data.input`
- `data.constant`
- `data.select`
- `control.if`
- `control.switch`
- `control.merge`

These should be deterministic and side-effect free.

### Algorithmic capability nodes

The runtime is also intended to execute ordinary algorithmic processing that does not require a model call.

Conceptual examples use a family such as:

- `algorithm.*`

These capabilities may cover deterministic transforms, analysis, filtering, scoring, conversion, geometry, signal/image operations, or other bounded library/runtime work.

Algorithm implementations should be versioned where output semantics matter. They remain subject to the same input/output, timeout, memory, and concurrency bounds as every other node.

### Vem capability nodes

Future Vem functionality should be connectable as registered capabilities once Vem exposes a stable callable contract.

Conceptually:

- `vem.*`

The workflow IR should not hard-code Vem internals or guess its eventual API. Vem-specific lifecycle, state, and implementation details should remain behind its adapter/contract.

### Service and external capability nodes

Candidates:

- `intelligence.request`
- `generation.submit`
- `external_ai.*` for explicitly registered local/remote AI services, including API-backed providers;
- `mcp.*` for explicitly registered external MCP capabilities;
- later explicitly registered product/service commands.

These are adapters to owning services or configured external capabilities.

External AI/API nodes must not carry raw credentials or private endpoints in the portable workflow. Provider configuration belongs in runtime/deployment configuration or the owning adapter.

External MCP calls must be backed by configured connections and registered tool schemas. A workflow must not be able to invent an arbitrary MCP endpoint or tool and gain access merely by naming it.

The runtime should not invent hidden semantics around an upstream service.

## Execution model

The first executor should prefer a DAG.

Cycles, loops, recursion, and dynamic fan-out add substantial complexity and resource risk. They should not be accepted merely because a graph format can represent them.

If looping is added later, it should have an explicit bounded construct such as:

- maximum iterations;
- bounded collection size;
- explicit exit condition;
- cancellation support.

## Side effects

Node metadata should make side effects visible before execution.

Examples of side effects:

- generating an asset;
- changing product state;
- calling a paid remote API;
- sending a message;
- writing to external storage;
- starting or stopping a runtime.

The compiler should be able to identify side-effecting nodes before the run starts.

A dry-run/compile operation should report them without executing them.

## Resource budgets

A workflow should be rejected before execution if its declared or inferred bounds exceed policy.

Likely limits include:

- serialized workflow size;
- node count;
- edge count;
- graph depth;
- parallelism;
- fan-out;
- per-node timeout;
- total run duration;
- per-node input/output bytes;
- total result size;
- event/log volume.

The exact defaults belong to implementation and tests, not this design document.

## Error model

Errors should be structured and stable.

Prefer errors such as:

- `unsupported_schema_version`
- `invalid_graph`
- `unknown_node_type`
- `invalid_reference`
- `capability_unavailable`
- `permission_denied`
- `budget_exceeded`
- `node_timeout`
- `run_cancelled`
- `upstream_failure`

Do not expose raw provider responses by default.

## Adaptive workflows

Adaptive graph changes are a later feature.

The safe model is not "let the AI replace arbitrary runtime state".

A future patch operation should be constrained to explicit graph-edit operations against a known workflow/run revision, for example:

- add node;
- remove an unexecuted node;
- replace configuration of an unexecuted node;
- add/remove dependency edge where valid;
- update selected output binding.

Already executed side effects must not be silently rewritten.

Patch provenance should record:

- previous graph revision;
- patch operations;
- caller;
- validation result;
- resulting revision.

## Persistence

The first implementation should not assume durable distributed execution.

A minimal in-process executor is a better starting point.

If durable run persistence is added later, runtime persistence owns **runtime execution state only**. It must not become a copy of Agent conversations, Generation assets, or product documents.

## Non-goals for the initial implementation

- arbitrary Python execution;
- shell execution;
- unrestricted HTTP requests;
- arbitrary filesystem access;
- credential transport inside workflow JSON;
- distributed scheduler;
- plugin marketplace;
- ComfyUI graph compatibility;
- LangGraph/n8n compatibility;
- long-lived Agent memory;
- generation asset ownership;
- product document ownership.

Those may be revisited only when a concrete requirement and security model exist.
