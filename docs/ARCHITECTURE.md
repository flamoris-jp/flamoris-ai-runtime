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

## Execution machines and continuation boundary

Inference and workflow share one Runtime Kernel, but they should not collapse into one implementation object.

The intended internal shape is:

```text
                    Runtime Kernel

          ┌────────────┴─────────────┐
          ▼                          ▼
   Inference Machine          Workflow Machine
          │                          │
          └──────────┬───────────────┘
                     ▼
              Job (scheduler-visible)
                     │
        waiting/paused owns optional
                     ▼
               Continuation
                     │
               resume state only

Scheduler schedules Jobs; Resource Manager accounts execution leases
and retained state footprints. All state transitions emit Events.
```

The **Inference Machine** owns model-adjacent lifecycle and backend state transitions.

The **Workflow Machine** owns compiled workflow control, dependency progression, joins/races, bindings, and plan state.

A **Continuation** is the explicit resume-state bridge for a scheduler-visible Job. It describes how that same Job may continue after a yield/wait without forcing Inference Machine and Workflow Machine into one implementation.

A Continuation is owned by exactly one waiting/paused Job and has no independent scheduler identity. The Job remains authoritative for cancellation, timeout, parent/child provenance, metrics, and terminal status.

A continuation may contain:

- owning Job and execution-machine identity;
- resume point;
- backend/state reference where supported;
- waiting condition or child-job set;
- result/input bindings;
- resource requirements;
- model/device/resource affinity hints;
- deadline/cancellation linkage inherited from the Job.

A suspended Job should not normally retain a physical **execution lease** while waiting. However, a backend/state reference may still keep KV cache or other state resident in VRAM/RAM. That **retained state footprint** remains allocated and must be accounted by the Resource Manager until offload, snapshot/migration, or eviction actually frees the resource. Lease release alone is never evidence that memory was freed.

## Workflow compilation

Workflow IR is input to compilation, not scheduler state.

```text
Workflow IR
    ↓
Schema / graph / reference validation
    ↓
Capability resolution and static policy/requirement analysis
    ↓
Execution Plan Compiler
    ↓
Execution Plan
    ├─ steps
    ├─ dependencies
    ├─ bindings
    ├─ resource requirements
    ├─ effect sets
    ├─ side-effect boundaries
    ├─ limits
    └─ potential suspension sites / continuation policy
    ↓
Workflow Machine / Jobs / Scheduler
```

The scheduler must not directly interpret arbitrary Workflow JSON.

The compiled plan is the Runtime-owned normalized execution contract. This separates AI-authored or externally supplied declarative intent from the bounded structures used for execution.

Compilation may determine what permissions/effects/resources would be required, but it does **not** grant durable authorization. Current capability availability, caller authorization, budgets, and policy must be revalidated when a run is admitted for execution. Side-effecting dispatch must also be revalidated immediately before execution when required by policy. Reusing or caching an Execution Plan must never preserve stale permission.

The plan records statically known suspension sites and continuation policy. Concrete Continuation instances are live Runtime state and are created when a Job actually yields, including runtime-defined inference yields that could not be enumerated as concrete instances during compilation.

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

### Continuation state

May include:

- continuation ID and exactly one owning Job/run;
- owning execution machine;
- resume point;
- waiting condition;
- state/backend reference where supported;
- bounded resume bindings;
- resource requirements and affinity hints;
- deadline/cancellation linkage;
- validity/resume status derived from the owning Job lifecycle.

Continuation state is active Runtime state, not durable Agent memory. It is not independently queued, cancelled, timed out, or completed by the Scheduler.

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

### Execution Plan

An Execution Plan is produced by the compiler after Workflow IR validation and capability resolution.

It is distinct from both Workflow IR and live Job state.

The plan should contain only Runtime-approved, bounded execution semantics. It may include resolved capability versions, dependencies, bindings, effect sets, resource requirements, limits, side-effect boundaries, and statically known potential suspension sites/continuation policy.

The plan does not contain live Continuation instances and does not carry an authorization grant.

### Continuation

A Continuation is resumable execution state owned by a Job, not a Job subtype and not a scheduler-visible work item.

A Job may yield and create/update its Continuation. When the waiting condition is satisfied, the Runtime/Scheduler transitions the **same owning Job** from `waiting`/`paused` back toward `queued`/runnable state; the Job resumes using the Continuation. The Job ID and lifecycle authority do not change across this suspension.

This allows an InferenceJob to wait for Vision, algorithm, MCP, or other child work without treating every pause as an ad-hoc special case inside the inference implementation or creating a second lifecycle authority.

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
- effect set;
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

- what counts as a winner, including an explicit acceptance rule where applicable;
- whether failure can win;
- what happens to unfinished losers;
- whether losers are cancelled;
- whether completed loser results may be retained for cache/provenance;
- how already-completed side effects are treated.

Cancellation of losers is not equivalent to rollback.

Later, race may support speculative execution where a fast provisional result is emitted before a stronger candidate finalizes. That requires explicit `provisional`, `final`, and `superseded` event/result semantics and is not part of the first race implementation.

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

## Event journal and trace replay

Structured events should optionally be retained as a bounded event journal.

Two concepts must remain separate:

- **trace replay** - replay/inspect the recorded event sequence without executing capabilities again;
- **re-execution/retry** - submit work again under explicit runtime and side-effect rules.

A trace replay must never recreate external writes, paid calls, destructive operations, or other side effects.

This separation allows regression tests to verify lifecycle, dependency order, continuation behavior, resource allocation decisions, interrupts, and effect ordering even when model text is nondeterministic.

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

## Effect model

Capability effects should be represented as a composable set rather than one boolean or one mutually exclusive enum.

Initial conceptual effects:

- `pure` - depends only on declared immutable inputs and produces declared outputs, with no ambient state access, external authority crossing, mutation, destructive behavior, or cost;
- `read` - reads ambient or mutable state beyond declared immutable inputs;
- `write` - mutates persistent or externally observable state;
- `external` - crosses an external process/service/network authority boundary;
- `destructive` - deletes or irreversibly/restrictively mutates state;
- `paid` - may incur monetary, billed-token, quota, or similarly policy-relevant external cost.

Initial effect-set validation rules:

- every capability declares a non-empty known effect set;
- `pure` is exclusive with all other effects;
- `read` and `write` may coexist;
- `destructive` requires `write`;
- `external` is orthogonal to `read`, `write`, `destructive`, and `paid`;
- `paid` is orthogonal to `read`, `write`, `destructive`, and `external`;
- unknown or contradictory combinations fail closed rather than being coerced to `pure`.

Examples:

```text
algorithm.resize          -> { pure }
vision.describe           -> { read }
mcp.github.create_issue   -> { external, write }
generation.image          -> { external, paid }
file.delete               -> { write, destructive }
```

The compiler should derive a run-level effect summary before execution so authorization, user confirmation, budget checks, and side-effect ordering can operate on machine-readable data.

Workflow IR cannot self-declare itself safe; effect metadata comes from the registered capability.

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

Model residency is a first-class scheduling input. The Resource Manager may track which logical model/backend is resident or warm, estimated memory pressure, approximate eviction/reload cost, and retained per-Job/Continuation state footprints.

An **execution lease** represents permission/capacity for active execution. A **retained state footprint** represents memory/state still resident while a Job is suspended. Releasing a lease does not release a footprint. The Scheduler must consider both before admitting another GPU/RAM-heavy Job. The scheduler may reorder otherwise-ready jobs when doing so preserves semantics and materially reduces model switching cost.

Residency optimization must never override dependency order, effect ordering, fairness/budget limits, cancellation, or explicit priority policy.

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

## Implementation language and process boundary

The intended Runtime Kernel implementation language is **C++**.

This choice is architectural rather than cosmetic. The kernel is expected to own or closely coordinate:

- decode/prefill lifecycle control;
- model/backend state and cache lifetime;
- low-latency interrupt points;
- continuation state transitions;
- scheduling and resource decisions;
- native runtime/backend integration;
- bounded event emission.

The Runtime Kernel should remain headless and transport-independent.

```text
MCP adapter ─┐
API adapter ─┼──► C++ Runtime Kernel
CLI adapter ─┤
bindings  ───┘
```

External capabilities do not need to be rewritten in C++. Generation, MCP, remote AI/API, and other services may remain out-of-process behind registered capability adapters.

The exact minimum C++ standard, compiler/toolchain support, library layout, exception/error policy, ABI strategy, and optional language bindings are Phase 0 decisions. They must be based on current `flamoris-LLM` code and representative runtime research rather than guessed in advance.

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
