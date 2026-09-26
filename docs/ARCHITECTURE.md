# Architecture

## Purpose

FLAMORIS AI Runtime is intended to be a **model-adjacent execution runtime that controls inference and workflow in one runtime kernel**.

It is not merely a workflow service layered above an opaque LLM endpoint.

The Runtime should own enough of the model execution lifecycle to observe, pause, interrupt, resume, and coordinate inference with other jobs while preserving backend state where possible.

A persistent Agent may sit above it, but the Runtime itself is responsible for the active execution machinery.

```text
Caller / Agent / App
        │
        ▼
FLAMORIS AI Runtime
┌──────────────────────────────┐
│ Runtime Kernel               │
│  ├─ Inference Controller     │
│  ├─ Workflow Engine          │
│  ├─ Job Scheduler            │
│  ├─ Event / Trace Bus        │
│  ├─ Resource Manager         │
│  └─ Capability Registry      │
└──────────────┬───────────────┘
               │
     ┌─────────┼──────────┐
     ▼         ▼          ▼
   Model    local work  external work
```

## Responsibility boundary

### Runtime owns

Planned ownership:

- model-adjacent inference lifecycle;
- model/backend selection contracts;
- prefill/decode/sampling control where supported;
- active inference state;
- runtime workflow IR and validation;
- job creation, lifecycle, dependency tracking, and scheduling;
- logical parallelism and resource-aware physical scheduling;
- pause, resume, interrupt, and cancellation semantics;
- structured execution events and bounded trace/log output;
- capability discovery and execution adapters;
- per-run limits, provenance, and result assembly.

### Runtime does not own

The Runtime should not become the durable authority for:

- Agent identity, personality, conversation history, or long-term memory;
- Studio account/UI state;
- desktop product documents or editing history;
- generated-media asset ownership that belongs to Generation;
- host-wide GPU/service lifecycle policy that belongs to GPU Node Manager;
- arbitrary shell, filesystem, network, credential, or code execution.

A capability may call another FLAMORIS service without taking ownership of that service's domain state.

## Relationship to Intelligence MCP

Do not assume that model execution internals belong outside this repository merely because Intelligence MCP exists.

The intended boundary is:

- **AI Runtime** owns the active model execution/inference machinery for models it directly hosts or controls;
- **Intelligence MCP** may expose or route bounded intelligence capabilities to callers, depending on the final ecosystem integration;
- an MCP/API surface must not force the Runtime to surrender the inference control points needed for pause, interrupt, state preservation, or workflow handoff.

The exact service integration remains to be finalized during implementation.

## Runtime kernel

The runtime kernel coordinates four tightly related concerns:

```text
             Runtime Kernel
       ┌──────────┼──────────┐
       ▼          ▼          ▼
   Inference   Workflow     Jobs
       │          │          │
       └──────┬───┴────┬─────┘
              ▼        ▼
          Event Bus   Resources
```

These concerns are separate contracts, but they must cooperate without forcing every transition through an external request/response boundary.

## Model/backend layer

A model backend should expose only the execution primitives the Runtime actually needs.

Potential responsibilities include:

- model loading;
- tokenization;
- prompt/prefill;
- decode iteration;
- logits access required for sampling;
- sampling support or sampler integration;
- cache/state management;
- state capability reporting;
- optional state snapshot/rewind operations;
- backend-specific metrics.

The public Runtime contract should not depend on CUDA, ROCm, OpenCL, Vulkan, or one model architecture.

At the same time, backend abstraction must not erase useful execution control.

A backend that only offers an opaque `generate(prompt) -> text` call may be usable as a limited capability, but it cannot provide the same pause/interrupt/state-preserving semantics as a backend exposing decode-level control.

## Inference lifecycle

Conceptual lifecycle:

```text
created
  ↓
loading / ready
  ↓
prefill
  ↓
decoding
  ├─ pause
  ├─ interrupt
  ├─ wait for child jobs
  ├─ resume
  └─ continue decode
  ↓
terminal
  ├─ succeeded
  ├─ cancelled
  └─ failed
```

The exact states may change during implementation.

### Decode control point

The preferred control boundary is around a decode iteration:

```text
model step
  ↓
logits
  ↓
constraints / sampling
  ↓
next token
  ↓
update model state
  ↓
emit bounded events
  ↓
apply interrupt / workflow decision
  ↓
next iteration
```

This allows low-latency observation and intervention without requiring tensor-level instrumentation.

## Runtime state

Separate durable Agent state from active execution state.

### Active inference state

May include:

- backend/model handle;
- token sequence/position;
- KV or equivalent cache;
- sampling configuration/state;
- context bindings;
- stop conditions;
- current workflow/run link;
- active child jobs;
- interrupt flags;
- metrics.

### Workflow run state

May include:

- workflow revision;
- validated execution plan;
- node/job mapping;
- dependency state;
- produced values/references;
- side-effect provenance;
- resource budget;
- run status.

### Durable Agent state

Not owned here:

- identity;
- long-term memory;
- personality;
- durable conversation history;
- user/project preferences.

## Workflow versus Job

These are deliberately different concepts.

### Workflow

A workflow describes:

- dependencies;
- data flow;
- control flow;
- requested capabilities;
- result bindings;
- limits.

### Job

A job is a scheduler-visible unit of active work.

A workflow node may create one job, no job, or later a bounded set of jobs depending on the node contract.

Examples:

- one inference segment -> `InferenceJob`;
- one image analysis operation -> `VisionJob`;
- one pure transform -> `AlgorithmJob`;
- one remote tool invocation -> `McpJob`.

The mapping must remain explicit enough for cancellation, tracing, provenance, and resource scheduling.

## Job lifecycle

Conceptual common states:

```text
created
  ↓
queued
  ↓
running
  ├─ waiting
  ├─ paused
  ├─ cancelling
  └─ running
  ↓
terminal
  ├─ succeeded
  ├─ cancelled
  └─ failed
```

Not every job type must support every transition.

Capability/job metadata should declare at least:

- cancellable;
- pausable/resumable;
- idempotent;
- side-effecting;
- resource class;
- retry policy;
- expected/maximum output size;
- timeout policy.

## InferenceJob

`InferenceJob` is a specialized job that may hold model execution state across pauses and child-job waits.

Conceptually:

```text
InferenceJob
├─ model/backend
├─ cache/state
├─ token position
├─ sampling state
├─ interrupt state
├─ workflow/run linkage
└─ child jobs
```

A Runtime implementation should not promise pause/resume or rewind for a backend that cannot safely preserve or mutate its state.

These are advertised capabilities, not universal assumptions.

## Job Scheduler

The scheduler decides when runnable jobs actually execute.

It must distinguish:

- logical dependency readiness;
- caller/run concurrency limits;
- CPU capacity;
- GPU/device capacity;
- model residency;
- VRAM/RAM pressure;
- exclusive resources;
- remote rate limits;
- side-effect constraints.

### Logical parallelism

Independent jobs may be logically parallel:

```text
        ┌─ Job A ─┐
input ──┤         ├─ join
        └─ Job B ─┘
```

### Physical scheduling

Logical parallelism does not guarantee simultaneous device execution.

For example, two GPU-heavy jobs may be serialized while a CPU transform and remote MCP call run concurrently.

Workflow semantics should not encode one specific GPU topology.

## Join

`join` waits for a defined set of jobs/results.

It should have explicit failure semantics, for example:

- fail if any required child fails;
- collect partial results;
- ignore explicitly optional children.

The first implementation should keep these policies small and deterministic.

## Race

`race` allows multiple candidate jobs to proceed and selects the first result meeting an explicit acceptance rule.

Conceptual example:

```text
            ┌─ local model ───────┐
request ────┼─ remote specialist ─┼─ race ─► selected result
            └─ cached path ───────┘
```

Race semantics must define:

- what counts as a winner;
- whether failure can win;
- what happens to unfinished losers;
- whether losers are cancelled;
- whether completed loser results may be retained for cache/provenance;
- how already-completed side effects are treated.

Cancellation of losers is not equivalent to rollback.

## Interrupt control

Interrupt is a first-class Runtime mechanism.

Conceptual actions:

- `stop`
- `pause`
- `resume`
- `cancel`
- inject bounded external input;
- alter an unexecuted workflow path;
- later, request rewind when the backend explicitly supports it.

The Runtime should record both:

1. when the interrupt was requested;
2. when and where it was applied.

This allows real-time UI to distinguish "stop requested" from "stopped".

## Workflow and inference interaction

The Runtime should support inference yielding to workflow work and later continuing.

```text
InferenceJob
   ↓
workflow decision / external need
   ↓
yield
   ↓
Job Scheduler
   ├─ child job A
   ├─ child job B
   └─ child job C
   ↓
await / join / race
   ↓
bounded result injection
   ↓
InferenceJob resume
```

Result injection must be explicit. Large media should normally flow as handles/references instead of being copied repeatedly through text context.

## Event and trace architecture

Structured events should be the primary observability mechanism.

```text
Runtime components
      │
      ▼
   Event Bus
  ┌───┼───────────────┐
  ▼   ▼               ▼
logs  live stream   test probes
                    / Studio
```

Potential event families:

### Inference

- `inference.started`
- `prefill.started`
- `prefill.completed`
- `decode.iteration`
- `token.generated`
- `sampling.completed`
- `inference.paused`
- `inference.resumed`
- `inference.completed`

### Jobs

- `job.submitted`
- `job.queued`
- `job.started`
- `job.progress`
- `job.waiting`
- `job.completed`
- `job.failed`
- `job.cancelled`

### Workflow

- `workflow.started`
- `workflow.node.ready`
- `workflow.node.started`
- `workflow.node.completed`
- `workflow.completed`

### Control

- `interrupt.requested`
- `interrupt.applied`
- `race.started`
- `race.winner_selected`
- `join.completed`

Event schemas should be stable enough for live UI and tests.

## Logging levels and sensitive data

Observability should be configurable.

Suggested conceptual levels:

1. lifecycle/state/timing;
2. token and sampling information;
3. model-exposed reasoning channel where intentionally available;
4. deep backend debug probes.

Normal operation must not log:

- credentials;
- arbitrary private provider payloads;
- unbounded tensors;
- unbounded media;
- unrestricted filesystem contents.

Event volume itself must be budgeted.

## Capability registry

A capability entry should eventually describe:

- stable identifier;
- version;
- input schema;
- output schema;
- side effects;
- idempotency;
- cancellation support;
- pause/resume support where meaningful;
- resource class;
- permission requirements;
- bounded output characteristics;
- availability.

The registry is not permission by itself.

## Resource management

Resource management becomes important once inference and workflow jobs coexist.

The Runtime should eventually reason about:

- CPU;
- RAM;
- GPU device;
- VRAM;
- model residency;
- I/O;
- network/API concurrency;
- external provider limits.

The first implementation may use a simple scheduler, but the contracts should not assume unlimited physical parallelism.

## Headless first

Execution semantics must not depend on GUI state.

A future visual interface may inspect:

- current inference stage;
- generated tokens;
- active jobs;
- queue state;
- dependencies;
- race participants;
- resource use;
- interrupt state;
- event history.

Canvas positions and visual metadata remain non-semantic.

## Error model

Prefer structured stable errors such as:

- `unsupported_model`
- `backend_unavailable`
- `invalid_workflow`
- `unknown_capability`
- `invalid_reference`
- `permission_denied`
- `budget_exceeded`
- `resource_unavailable`
- `job_timeout`
- `job_cancelled`
- `inference_interrupted`
- `upstream_failure`

Do not expose raw provider responses by default.

## Implementation foundation

Before implementation, evaluate `flamoris-net/flamoris-LLM` as a concrete foundation.

Potentially reusable areas include:

- explicit model runtime boundary;
- generation loop;
- cache handling;
- tokenizer/model contracts;
- compute boundary;
- CPU reference path;
- OpenCL backend experiments;
- test discipline.

The new Runtime should not inherit model-specific code into unrelated workflow layers by accident.

A likely evolution is:

```text
existing FLAMORIS LLM
  └─ model / compute / inference foundation
             │
             ▼
FLAMORIS AI Runtime
  ├─ Inference Controller
  ├─ Job Scheduler
  ├─ Workflow Engine
  ├─ Event Bus
  ├─ Interrupt Control
  └─ Capability System
```

## Runtime research before implementation

Before freezing the backend/inference interfaces, study representative runtimes/stacks including:

- `llama.cpp`;
- Hugging Face Transformers;
- vLLM;
- TensorRT-LLM.

Compare at least:

- prefill/decode structure;
- KV/cache lifetime;
- streaming;
- sampler hooks;
- cancellation;
- pause/resume;
- scheduler design;
- continuous batching;
- state mutation/rewind;
- observability/statistics;
- server API versus embedded-runtime control.

This research should produce a short decision record explaining which control points FLAMORIS must own directly.

## Non-goals for the first implementation

- distributed cluster scheduling;
- arbitrary Python execution;
- unrestricted shell execution;
- unrestricted HTTP/filesystem authority;
- plugin marketplace;
- compatibility with every workflow format;
- universal rewind support;
- universal pause support across opaque remote providers;
- long-term Agent memory;
- training/fine-tuning infrastructure unless separately adopted.
