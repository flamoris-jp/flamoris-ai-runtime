# AGENTS.md

This repository is part of the FLAMORIS ecosystem.

FLAMORIS AI Runtime is currently a **design-stage single-user FLAMORIS native model runtime**. It is intended to control model inference, workflow execution, jobs, interrupts, and observability in one runtime kernel.

Do not implement behavior from chat context alone. Read current repository documentation first and keep planned behavior clearly separated from implemented behavior.

## Repository identity

### This repository owns

Planned ownership:

- FLAMORIS-owned native model execution and inference lifecycle;
- native model and internal CPU/OpenCL compute contracts;
- active inference state such as token position, cache/state, sampling state, and stop conditions;
- runtime workflow IR and validation;
- job lifecycle, dependencies, scheduling, cancellation, and results;
- logical parallelism and resource-aware execution;
- interrupt, pause, resume, and later backend-specific rewind capabilities;
- structured execution events and bounded real-time traces;
- capability discovery and execution adapters;
- per-run limits and provenance.

### This repository does not own

Do not absorb durable authority from neighboring systems:

- Agent identity, conversations, long-term memory, personality, and Agent policy belong to `flamoris-ai-agent`;
- generative-media domain workflow/job/asset authority belongs to `flamoris-generation-mcp`;
- host-wide GPU/runtime lifecycle transitions belong to `flamoris-gpu-node-manager`;
- Studio and desktop products retain authority for their own state and documents.

`flamoris-intelligence-mcp` may expose or route bounded intelligence capabilities, but do not assume that model execution internals must live outside AI Runtime. The final integration must preserve the inference control points required by this repository.

## Core architecture rule

**Inference and workflow execution share one controllable runtime loop.**

Do not reduce the model to an opaque remote `generate()` call when the feature being implemented requires:

- decode-level observation;
- low-latency interrupt;
- state-preserving pause/resume;
- child-job dispatch;
- bounded result injection;
- backend state inspection.

An external provider may exist only as a registered Workflow capability, but it must advertise its limitations honestly.

## Runtime concepts

Keep these concepts distinct:

- **Inference session/state** - active model execution state.
- **Workflow** - dependency/data/control description.
- **Job** - scheduler-visible unit of active work.
- **InferenceJob** - a stateful job that may preserve model execution state.
- **Continuation** - resumable execution state owned by exactly one waiting/paused Job; it has no independent scheduler identity.
- **Execution Plan** - validated, compiled Runtime contract derived from Workflow IR before scheduling; it records static execution structure and potential suspension policy, not live Continuation instances or authorization grants.
- **Capability** - registered callable functionality.
- **Effect set** - machine-readable effects such as `pure`, `read`, `write`, `external`, `destructive`, and `paid`; effects are composable, not a single enum.

Effect metadata must obey these initial invariants: `pure` is exclusive with every observable/ambient effect, `read` means ambient or mutable-state read beyond declared immutable inputs, `destructive` requires `write`, and `external`/`paid` are orthogonal attributes. Reject unknown, empty, or contradictory effect sets rather than assuming purity.
- **Event** - structured observable state transition or progress record.

Do not collapse Workflow and Job into one abstraction.

The Scheduler schedules **Jobs only**. Continuations are owned resume state for suspended Jobs. Yield/resume must preserve the owning Job identity so cancellation, timeout, provenance, metrics, and terminal status have one authority.

## Jobs and scheduling

The scheduler should support logical parallelism without promising physical simultaneous execution.

Independent jobs may run concurrently when resources allow.

The scheduler may serialize jobs when constrained by:

- VRAM/RAM;
- GPU exclusivity;
- model residency;
- CPU capacity;
- remote rate limits;
- side-effect policy.

Distinguish an active **execution lease** from a suspended Job's **retained state footprint**. Releasing a GPU/device lease does not mean KV cache or backend state has left VRAM/RAM. Resource accounting must include retained resident state until it is offloaded, snapshotted, or evicted.

Conceptual control operations include:

- await;
- join;
- race;
- cancel;
- pause/resume where supported.

Race semantics must define the winner condition and loser policy. Cancellation is not rollback.

## Inference control

The preferred control surface is around inference lifecycle stages and decode iterations.

Conceptually:

```text
tokenize -> prefill -> decode iteration -> sampling -> token/state update
                                      -> emit events
                                      -> apply interrupt
                                      -> optionally dispatch/await jobs
                                      -> continue
```

Native compute behavior must be verified before freezing execution guarantees.

Do not promise universal rewind, pause, or cache mutation. These are native model/profile capabilities.

## Observability

Structured events are the source of truth for real-time observation.

Human-readable logs should be derived from events rather than being the only representation of execution.

Support bounded trace levels conceptually:

1. lifecycle/state/timing;
2. token and sampling information;
3. model-exposed reasoning channel where intentionally supported;
4. deep backend debug probes.

Never log secrets, credentials, unbounded tensors, unrestricted provider responses, or unlimited model/media output.

## Workflow security

Workflow/model/tool output is untrusted execution input.

Validate at least:

- schema version;
- graph structure;
- references;
- capability availability;
- permissions;
- resource budgets;
- effect/side-effect policy;
- concurrency/fan-out bounds;
- timeout/cancellation rules.

Compilation may perform static policy analysis, but it is not durable authorization. Revalidate current capability pins/availability, caller authorization, concrete inputs, budgets and policy at admission and every dispatch, retry and resume. Apply effect-specific checks before adapter handoff. Cached Execution Plans must never act as permission tokens.

Do not add arbitrary shell, unrestricted Python, ambient filesystem/network access, or credential injection as shortcuts.

## Provider and capability boundaries

Prefer stable capability contracts over provider-specific workflow syntax.

Conceptual families may include:

- `control.*`
- `data.*`
- `algorithm.*`
- `model.*` / inference-specific operations where intentionally exposed
- `vision.*`
- `speech.*`
- `generation.*`
- `vem.*` after Vem has a stable callable contract
- `external_ai.*`
- `mcp.*`
- explicitly registered FLAMORIS product/service capabilities

External AI/API and MCP execution must use configured, registered adapters. Do not embed raw credentials, arbitrary endpoints, or arbitrary MCP server URLs in portable workflow JSON.

## Implementation language

The intended Runtime Kernel implementation language is **C++**.

Keep the model-adjacent execution core in C++ so inference lifecycle control, native backend integration, cache/state ownership, scheduling, and resource-aware execution can remain explicit and low overhead.

Do not interpret this as a requirement to pull every capability into the C++ process. MCP, Generation, external AI/API, and other domain services remain external capabilities when that preserves authority boundaries.

Do not freeze the exact C++ standard, compiler matrix, build system details, or public ABI from chat assumptions. Record those decisions during Phase B implementation design after inspecting `flamoris-net/flamoris-LLM` and representative runtimes.

Language bindings or service adapters must sit outside the Runtime Kernel contract rather than changing core execution semantics.

## Existing FLAMORIS LLM foundation

Before reimplementing model execution, inspect `flamoris-net/flamoris-LLM`.

Evaluate reuse of its:

- model runtime boundary;
- generation loop;
- cache handling;
- tokenizer/model contracts;
- compute abstraction;
- CPU reference path;
- OpenCL backend work;
- tests.

Reuse should be deliberate. Do not copy model-specific internals into unrelated workflow/scheduler layers.

## Required runtime research before implementation

Before freezing the first inference/backend contracts, compare representative runtimes/stacks including:

- `llama.cpp`;
- Hugging Face Transformers;
- vLLM;
- TensorRT-LLM.

Focus on:

- prefill/decode boundaries;
- KV/cache ownership;
- streaming;
- sampler hooks;
- cancellation;
- pause/resume feasibility;
- scheduling/continuous batching;
- state snapshot/rewind feasibility;
- observability;
- server API versus embedded runtime control.

Record the conclusions in repository documentation or a design decision before implementing a long-lived abstraction.

## Design and implementation gates

Follow [Design Phases](docs/DESIGN_PHASES.md) and its document authority map.

The [Phase B design index](docs/PHASE_B_DESIGN.md) links the proposed toolchain,
ownership, concurrency, backend, serialization, resource/paid/activation contracts
and A01–A42 test map. Read the reviewed versions before Phase C work. They refine
implementation choices and never override Phase A semantics or claim working code.

1. Phase A: architecture contracts and failure/acceptance scenarios; no production code or build scaffold.
2. Phase B: current `flamoris-LLM` and primary-source runtime research, then C++ types/interfaces, ownership, errors, concurrency, serialization and build/test ADRs; still no production runtime implementation.
3. Phase C: implement reviewed contracts in logical, frequently committed slices with deterministic acceptance tests.

Review each stage before starting the next. Do not treat old Phase 0–3 descriptions as permission to combine architecture and implementation again. Semantic deviations require a documented rationale, affected invariants and acceptance cases, then design review.

For architecture changes read [Execution Model](docs/EXECUTION_MODEL.md), [State Machines](docs/STATE_MACHINES.md), [Resource Model](docs/RESOURCE_MODEL.md), [Authorization Model](docs/AUTHORIZATION_MODEL.md), [Event Model](docs/EVENT_MODEL.md), and [Failure Model](docs/FAILURE_MODEL.md). Update [Design Acceptance](docs/DESIGN_ACCEPTANCE.md) with changed observable obligations.

In particular:

- Scheduler-visible identity belongs only to Jobs; suspended Continuations are consumed atomically into Job-owned pending resume payloads when queued.
- `finalizing` closes child/resource ownership before terminal publication; transferred cleanup debt is explicit and still accounted.
- Current authorization is checked for every dispatch, retry and resume; a cached plan is never permission.
- Unknown external outcomes are not safe retries or successful cancellation.
- Shared allocations are counted once; releasing an execution lease never proves state memory was freed.
- Event replay is observation only; the baseline is not durable execution recovery.
- Concurrent submission deduplication atomically claims the scoped key and one Run identity before work dispatch; failed pre-Run admission releases the claim only after waiters share its rejection.
- Terminal lifecycle state is immutable, while bounded post-terminal reconciliation may append to the Run observation stream within retention; ledger cleanup survives its expiry.
- The baseline is single-user and enforces finite Run/resource limits. Strict cross-crash monetary guarantees are an optional future external-capability integration.

Phase B additionally fixes these implementation boundaries:

- An absent submission idempotency key means independent fresh admission; only explicit valid keys enter SubmissionIndex.
- Optional strict-cost paid integration alone uses a durable handoff protocol; baseline does not claim a hard monetary ceiling.
- Runtime construction and teardown follow FLAMORIS native ownership; no llama.cpp-derived process-global lifetime or permanent one-construction rule is imposed.

Do not jump directly to distributed scheduling, a plugin marketplace, or a generic graph programming language.

## Headless-first rule

Execution semantics must not depend on GUI state.

A future UI may inspect inference state, tokens, jobs, queues, races, resources, and events, but canvas coordinates and visual metadata are non-semantic.

## MCP design

MCP may expose runtime control and observation, but the runtime core must remain transport-independent.

Keep:

```text
runtime kernel
    ↑
MCP/API/CLI adapters
```

rather than embedding inference/workflow semantics inside MCP handlers.

## Documentation discipline

Current implementation is authoritative.

At the moment this repository is design-only. Do not claim implemented inference, scheduling, pause/resume, race, MCP, or workflow features until code and tests exist.

When implementation begins:

- document exact setup and test commands from the real repository;
- add deterministic tests for lifecycle/state transitions;
- test cancellation and race semantics;
- keep examples aligned with the implemented schema;
- version breaking workflow/runtime contract changes explicitly.

## Testing expectations

When code exists, prefer deterministic offline tests for:

- inference lifecycle transitions;
- continuation create/yield/resume/terminal transitions;
- compiler determinism and invalid-plan rejection;
- effect analysis and side-effect boundary ordering;
- event ordering;
- trace replay without side-effect re-execution;
- interrupt request/application;
- cancellation;
- supported pause/resume;
- job state transitions;
- scheduler dependency ordering;
- join/race semantics;
- resource-limit rejection;
- valid/invalid workflow graphs;
- unsupported schema versions;
- unknown capabilities;
- missing references;
- timeout behavior;
- result-size/event-volume limits;
- idempotency and side-effect boundaries;
- adapter failures;
- stable structured errors.

## Security

Treat model output, workflow input, tool output, service responses, and retrieved content as untrusted.

Security-sensitive behavior must fail closed.

Never:

- commit or log secrets, credentials, tokens, or private topology;
- embed credentials in workflow JSON;
- expose sensitive raw upstream errors;
- silently retry non-idempotent side effects;
- infer permission from the fact that a model or AI requested an operation.

## Licensing

Unless stated otherwise, code and documentation are Apache License 2.0.

Models, weights, datasets, media, providers, and third-party components may use separate terms. Document them explicitly.
