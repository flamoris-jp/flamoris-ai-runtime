# Runtime Concept

## Status

**Design concept only. No runtime is implemented yet.**

The review gates and normative document map are in [Design Phases](DESIGN_PHASES.md). This document describes intent; the detailed contracts specify the initial bounded behavior.

FLAMORIS AI Runtime is intended to be a **single-user FLAMORIS native model runtime**.

It owns model execution directly. Third-party runtimes, if ever called, are external Workflow capabilities, not inference backends.

The central idea is:

> **Inference and workflow execution share one controllable runtime loop.**

The Runtime should own enough of the inference lifecycle to observe it, interrupt it, pause it, resume it, dispatch other work, receive those results, and continue inference without unnecessarily rebuilding model state.

## The basic shape

```text
          App / Agent / ChatGPT
                  │
                  ▼
        FLAMORIS AI Runtime
        ┌───────────────────────┐
        │ Inference Controller  │
        │ Workflow Engine       │
        │ Job Scheduler         │
        │ Event / Trace Stream  │
        │ Capability Registry   │
        └───────────┬───────────┘
                    │
          ┌─────────┼──────────┐
          ▼         ▼          ▼
        Model    Local work   External work
                  │            │
                  ├─ algorithm ├─ MCP
                  ├─ Vision    ├─ API / external AI
                  ├─ audio     └─ FLAMORIS services
                  └─ future Vem
```

A persistent Agent may still live outside the Runtime:

- the Agent owns identity, conversation, durable memory, goals, and personality;
- the Runtime owns model execution state, inference control, workflow execution, jobs, cancellation, results, and execution events.

This means the Runtime can be used by `flamoris-ai-agent`, ChatGPT, Studio AI, or another authorized caller without becoming their persistent personality store.

## The Runtime owns the model execution path

FLAMORIS owns processor/tokenizer, model execution, cache/state, inference
steps and resource lifetime. CPU reference and OpenCL are its internal compute
implementations. The initial execution profile may be causal text; Vision,
audio, embedding and other native profiles must not inherit text-only state.

External AI/MCP calls run as registered Workflow capabilities under current
authorization and finite Run limits. Baseline deployment is single-user.

## Inference as a controllable loop

A decoder-style model commonly admits a lifecycle resembling:

```text
input
  ↓
tokenize
  ↓
prefill
  ↓
KV / model state
  ↓
┌──── decode iteration ────┐
│ model forward            │
│ logits                   │
│ sampling / constraints   │
│ next token               │
│ update inference state   │
│ emit events              │
│ check interrupt          │
│ optionally dispatch work │
└──────── repeat ──────────┘
```

The exact mechanics are native model/profile-specific and must be verified by implementation tests.

The architectural requirement is that FLAMORIS exposes explicit control points around these stages instead of reducing the entire operation to one opaque `generate()` call.

## Inference can call work and continue

The Runtime should support flows such as:

```text
Inference
   ↓
needs more information
   ↓
pause / yield
   ↓
spawn jobs
   ├─ Vision
   ├─ algorithm
   └─ MCP / external AI
   ↓
await / join / race
   ↓
inject bounded results
   ↓
resume inference
```

The important property is that the Runtime may preserve the relevant model/inference state across this handoff where the native execution profile supports it.

This is intended to reduce avoidable re-tokenization, model re-entry, request serialization, and loss of execution context while making the control flow explicit.

## Continuations are first-class resumable state

A **Continuation** represents resume state owned by exactly one scheduler-visible Job that is currently waiting or paused.

```text
Job
  = scheduler-visible lifecycle authority

Continuation
  = resume state + resume point + waiting contract owned by that Job
```

The Scheduler schedules Jobs only. Yield/resume preserves the Job identity; cancellation, timeout, provenance, metrics, and terminal state never migrate into a second Continuation lifecycle.

This prevents every yield from becoming an inference-specific special case without creating two scheduling authorities.

For example:

```text
Inference Machine
   ↓ yield
Continuation: waiting for Vision result
   ↓
Vision Job
   ↓ result
owning Job becomes ready to resume
   ↓
Inference Machine resumes
```

The same mechanism can connect Workflow Machine and Inference Machine without making them the same implementation.

A continuation may retain a native model/state reference where safe, but should not normally pin a physical **execution lease** while waiting. If that state remains resident in VRAM/RAM, its **retained state footprint** is still allocated and must remain visible to Resource Manager accounting. Resource affinity may be retained so the scheduler can prefer a warm model/device on resume.

## Jobs are first-class runtime work

A **Job** is the schedulable unit of execution.

Conceptual job classes include:

- `InferenceJob`
- `AlgorithmJob`
- `VisionJob`
- `SpeechJob`
- `GenerationJob`
- `ExternalAIJob`
- `McpJob`
- future Vem-backed jobs

An `InferenceJob` is special because it may retain model execution state such as:

- model/processor/execution-profile reference;
- token position;
- KV or equivalent cache state;
- sampling state;
- inference context;
- current workflow/run linkage;
- interrupt state;
- child job relationships.

Other jobs may be stateless, remote, CPU-bound, GPU-bound, I/O-bound, or side-effecting.

## Parallel, join, and race

Workflow dependencies should allow independent ready jobs to run concurrently.

The Runtime should distinguish **logical parallelism** from **physical simultaneous execution**.

For example:

```text
            ┌─ Vision Job ─────┐
input ──────┤                  ├─ join ─► inference
            └─ Metadata Job ───┘
```

The scheduler may execute both at once when resources allow, or serialize them when GPU memory, model residency, device exclusivity, or another resource policy requires it.

Useful control primitives include:

- **await** — wait for one job;
- **join** — wait for a defined set of jobs;
- **race** — accept the first qualifying result and apply an explicit policy to remaining jobs;
- **cancel** — request cancellation;
- **pause / resume** — where the job type supports it.

`race` is intentionally a runtime concept, not merely a UI feature. A future workflow may race a local model, a remote specialist, and a cached/retrieval path, then continue with the first result satisfying the configured success condition.

Race loser behavior is explicit. The baseline uses cancel_unfinished and excludes write/destructive participants; continuing losers for cache/provenance and speculative replacement are later extensions. No policy silently replays or rolls back side effects. See [Execution Model](EXECUTION_MODEL.md).

## Observable inference

The Runtime should make execution observable in real time.

Logs should be derived from structured runtime events rather than treating human-readable log strings as the only source of truth.

Conceptual events include:

- `inference.started`
- `prefill.started`
- `prefill.completed`
- `decode.iteration`
- `token.generated`
- `sampling.completed`
- `job.submitted`
- `job.started`
- `job.progress`
- `job.completed`
- `job.failed`
- `workflow.node.started`
- `workflow.node.completed`
- `interrupt.requested`
- `inference.paused`
- `inference.resumed`
- `inference.completed`

Consumers may include:

- console logging;
- JSONL logs;
- Studio live inspection;
- an event stream;
- MCP/API observation surfaces;
- test probes.

The event system must be bounded. Large tensors, unrestricted provider responses, secrets, and unbounded model output must not become ordinary logs.

## Inference trace levels

Observability should support explicit levels rather than one all-or-nothing debug stream.

Conceptually:

1. **Lifecycle** — timings, state changes, job transitions, resource use, stop reasons.
2. **Token / sampling** — generated token IDs/text and selected sampling metadata where enabled.
3. **Model-exposed reasoning stream** — only when the model/runtime intentionally exposes such a channel and the deployment policy permits recording it.
4. **Deep debug probes** — logits, selected activations, cache inspection, or native profile-specific diagnostics; disabled by default and strongly bounded.

The Runtime should not require deep internal tensor logging for normal operation.

## Interrupts are a core feature

A caller or operator may want to intervene while inference or workflow execution is still running.

The design should therefore support explicit interrupt requests.

Possible actions include:

- stop;
- pause;
- resume;
- cancel child jobs;
- inject new bounded input;
- redirect workflow control;
- later, rewind model state where a native execution profile safely supports it.

An interrupt request and the point where it actually takes effect are separate events. This distinction matters for debugging and UI feedback.

## Workflow IR is compiled before execution

Workflow IR is declarative input.

The Runtime should not hand raw Workflow JSON directly to the scheduler.

```text
Workflow IR
   ↓
Validator
   ↓
Execution Plan Compiler
   ↓
Execution Plan
   ↓
Workflow Machine
   ↓
Jobs / Continuations
   ↓
Scheduler
```

The compiler resolves registered capabilities, schemas, bindings, effects, limits, resource requirements, side-effect boundaries, and statically known suspension policy before execution.

Concrete Continuation instances remain live Runtime state and are created only when a Job actually yields. Inference may yield at runtime-defined control points that are not concrete Continuation instances in the compiled plan.

Compilation describes authorization and policy requirements but grants no permission. The Runtime revalidates current capability pins/availability, caller authorization, concrete input scope, budget and policy at admission and every dispatch, retry and resume. Effect-specific checks apply before adapter handoff.

This is particularly important when an AI generates Workflow IR: the AI may propose composition, while only the Runtime may turn validated composition into executable work, and only current Runtime policy may authorize execution.

## Workflow is inside the execution runtime

Workflow is not intended to sit above the Runtime as a completely separate orchestration service.

Instead:

```text
Runtime Kernel
   ├─ inference control
   ├─ workflow execution
   ├─ job scheduling
   ├─ event stream
   └─ resource management
```

Workflow describes dependencies and control.

Jobs are the scheduler-visible work units.

Inference is one important job type with deeper lifecycle integration.

This lets the same Runtime coordinate ordinary algorithms, model inference, Vision, audio, external AI, MCP, Generation, and future Vem capabilities without forcing every transition through a new top-level request.

## Open-ended composition, bounded execution

The broad composition goal remains:

> **Open-ended composition. Bounded execution.**

A workflow may combine registered capabilities across domains, but execution remains constrained by:

- capability registration;
- caller authorization;
- machine-readable effect sets;
- resource budgets;
- GPU/CPU/device policy;
- network/filesystem/credential boundaries;
- timeout and cancellation rules;
- event/logging limits;
- job concurrency policy.

## Relationship to the Agent

A useful split is:

```text
FLAMORIS AI Agent
  ├─ identity
  ├─ durable memory
  ├─ conversation
  ├─ goals
  └─ personality
        │
        ▼
FLAMORIS AI Runtime
  ├─ model inference
  ├─ inference state
  ├─ workflow
  ├─ jobs
  ├─ capabilities
  └─ execution events
```

The Runtime may perform reasoning through the loaded model, but it does not become the durable identity/memory authority of the Agent.

## Runtime Kernel implementation language

The intended Runtime Kernel implementation language is **C++**.

The goal is not "everything in one native binary." The goal is to keep model-adjacent control, state ownership, continuation handling, scheduling, and native model and compute integration in a kernel while preserving service boundaries for external capabilities.

```text
Agent / ChatGPT / Studio
          │
   MCP / API / CLI
          │
          ▼
    C++ Runtime Kernel
       ├─ Inference Machine
       ├─ Workflow Machine
       ├─ Continuations
       ├─ Jobs / Scheduler
       ├─ Event Journal
       └─ Resource Manager
          │
     registered capabilities
```

Python, C#, or other language integration may later exist as adapters/bindings if useful. Such bindings must not become the authority for Runtime execution semantics.

The exact C++ standard and toolchain are deliberately not frozen until Phase B research and inspection of `flamoris-net/flamoris-LLM`.

## Implementation research before freezing contracts

Before the first implementation freezes the inference API, study several modern runtimes and generation stacks, including at least:

- `llama.cpp`
- Hugging Face Transformers generation/cache APIs
- vLLM
- TensorRT-LLM

The research should compare:

- tokenize / prefill / decode boundaries;
- KV/cache ownership and lifetime;
- token streaming;
- sampling hooks;
- stop/cancel semantics;
- pause/resume feasibility;
- state snapshot/rewind feasibility;
- continuous batching/scheduling;
- request/job lifecycle;
- observability and iteration statistics;
- how much control is lost behind server-style APIs.

The purpose is not compatibility with all of them. It is to identify the smallest runtime control surface FLAMORIS needs.

## Existing FLAMORIS LLM work

`flamoris-net/flamoris-LLM` is a candidate implementation foundation because it already explores explicit model runtime, generation, cache, compute boundaries, and CPU/OpenCL execution.

Reusing it should be evaluated at the code/contract level rather than copied wholesale.

Model-specific code becomes a native model layer, while the new Runtime adds the inference controller, jobs, workflow execution, event stream, interrupts, and resource scheduling around it.

This is a design direction, not yet an implementation claim.

