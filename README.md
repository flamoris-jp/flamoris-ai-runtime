# FLAMORIS AI Runtime

**Inference and workflow, controlled in one runtime.**

FLAMORIS AI Runtime is a **single-user native model runtime** implemented in C++20.

It owns model execution, cache/state and inference control alongside jobs, workflows, interrupts and real-time observability. Native CPU reference and OpenCL compute are internal implementations. Third-party runtimes, when used, are registered Workflow capabilities. Strict cross-crash monetary limits are an optional future integration.

The core idea is:

> **Inference and workflow execution share one controllable runtime loop.**

> **Status: Phase C implementation and integration review.** See the
> [implementation evidence](docs/phase-c/STATUS.md), [build instructions](docs/BUILD.md),
> and [qualified native profile](fixtures/native/QUALIFICATION.md).

The native qualification fixture is a small, self-authored causal model used to
verify arithmetic and state control. It is not a pretrained language model or a
claim of general model-family support. The OpenCL qualification used a real POCL
CPU device; physical GPU and deployed host integration are separate gates.

Part of the [FLAMORIS AI ecosystem](https://github.com/flamoris-jp/flamoris-ai/blob/main/docs/ai-ecosystem.md).

## Build and try it

The current Phase C baseline is executable. Build with the reviewed CMake presets
described in [Build and validation](docs/BUILD.md). For the native qualification
fixture:

```sh
cmake --preset gcc-debug -DFLAMORIS_NATIVE_TESTS=ON
cmake --build --preset gcc-debug --parallel 2
ctest --preset gcc-debug --no-tests=error
```

The CLI currently exposes three entry points:

```text
flamoris-runtime validate FILE
flamoris-runtime serve
flamoris-runtime generate --prompt TEXT [--temperature FLOAT] [--seed UINT] [--max-tokens UINT]
```

`validate` compiles and validates a bounded submission and prints its plan
fingerprint. `serve` exposes the control protocol but deliberately has no local
host authority, so it remains fail-closed until a trusted embedding provides
capacity.

`generate` is an explicit **developer smoke-test path** for the self-authored
tiny native fixture. It runs the existing native `NativeSession` on the CPU and
emits JSONL events such as `token.generated` and `generation.completed`.
It is not a production model server, does not grant Runtime host capacity, and
does not establish support for pretrained model families.

Example:

```sh
./build/gcc-debug/flamoris-runtime generate \
  --prompt "Hello 日本語" \
  --temperature 0 \
  --seed 1 \
  --max-tokens 16
```

The fixture model defaults to `fixtures/native/tiny-causal-v1.bin` from the
source tree. Use `--model FILE` only for the same registered fixture format and
checksum contract.

## What it is

FLAMORIS AI Runtime is not only a workflow service above an already-finished LLM endpoint.

It is intended to own enough of model execution to coordinate:

- tokenize / prefill / decode / sampling lifecycle;
- active inference state and cache/state where supported;
- pause, resume, interrupt, and cancellation;
- Workflow IR validation and execution;
- schedulable Jobs;
- logical parallelism with resource-aware scheduling;
- `await`, `join`, and `race`;
- local algorithms and multimodal capabilities;
- external AI/API and MCP capabilities;
- structured real-time events and traces.

Conceptually:

```text
          App / Agent / ChatGPT
                  │
                  ▼
        FLAMORIS AI Runtime
        ┌───────────────────────┐
        │ Inference Controller  │
        │ Workflow Engine       │
        │ Job Scheduler         │
        │ Event / Trace Bus     │
        │ Resource Manager      │
        │ Capability Registry   │
        └───────────┬───────────┘
                    │
          ┌─────────┼──────────┐
          ▼         ▼          ▼
        Model    local work   external work
```

## Model-adjacent, not only orchestration

A normal model runtime may expose something conceptually like:

```text
prompt
  ↓
prefill
  ↓
decode
  ↓
sampling
  ↓
tokens
```

FLAMORIS AI Runtime wants explicit control points inside that lifecycle.

Conceptually:

```text
prefill
  ↓
decode iteration
  ↓
sampling
  ↓
token/state update
  ↓
emit event
  ↓
interrupt?
  ↓
workflow/job work?
  ↓
continue inference
```

This allows the Runtime to observe inference in real time, pause it, dispatch other work, wait for results, inject bounded results, and continue where the native execution profile supports state preservation.

## Workflow inside the Runtime

Workflow is part of the Runtime execution machinery.

It is not intended to be a completely separate orchestrator that repeatedly calls the model from outside.

```text
InferenceJob
   ↓
needs additional work
   ↓
yield / pause
   ↓
child jobs
   ├─ Vision
   ├─ algorithm
   ├─ external AI
   └─ MCP
   ↓
await / join / race
   ↓
selected bounded result
   ↓
resume inference
```

The goal is to avoid unnecessary model re-entry, serialization, and loss of execution state while keeping execution bounded and observable.

## Jobs

A **Job** is the scheduler-visible unit of active work.

Conceptual job types include:

- `InferenceJob`;
- `AlgorithmJob`;
- `VisionJob`;
- `SpeechJob`;
- `GenerationJob`;
- `ExternalAIJob`;
- `McpJob`;
- future Vem-backed jobs.

An `InferenceJob` may retain model execution state such as token position, cache/state, sampling state, workflow linkage, and interrupt state.

Not every Job/native execution profile must support pause, resume, rewind, or cancellation. These are capabilities, not assumptions.

## Continuations

A **Continuation** is Runtime-owned resume state attached to exactly one suspended Job.

Keep the distinction explicit:

```text
Job
  = the only scheduler-visible unit of work and lifecycle authority

Continuation
  = resume state owned by a waiting/paused Job
```

The Scheduler schedules Jobs, not Continuations. A yield does not create a second scheduler identity. When the wait is satisfied, the Runtime atomically consumes the Continuation into a Job-owned pending resume payload and queues the same Job. Its state remains accounted while resources are unavailable. See [State Machines](docs/STATE_MACHINES.md).

Cancellation, timeout, parent/child provenance, metrics, and terminal status remain properties of the Job/run lifecycle.

A continuation may include:

- the owning Job and execution machine;
- resume point;
- native model/state reference where supported;
- what result or event it is waiting for;
- input/result bindings required on resume;
- resource requirements and affinity hints;
- deadline/cancellation linkage inherited from the owning Job.

A paused Job should not normally hold an **execution lease** indefinitely. However, releasing an execution lease does not imply that retained KV/cache/native state has left VRAM or RAM. The Resource Manager must continue accounting for any **retained state footprint** until that state is offloaded, snapshotted elsewhere, or evicted.

Inference is not the only possible source of resumable state. Both the Inference Machine and Workflow Machine may suspend their scheduler-visible Job through the same Continuation contract.

## Parallel, join, and race

Independent jobs may become runnable at the same time.

```text
        ┌─ Job A ─┐
input ──┤         ├─ join ─► continue
        └─ Job B ─┘
```

Logical parallelism does not guarantee physical simultaneous execution.

The scheduler may serialize GPU-heavy jobs because of VRAM, model residency, or device policy while allowing CPU or remote jobs to run concurrently.

### Race

`race` allows multiple candidate jobs to compete:

```text
            ┌─ local model ───────┐
request ────┼─ remote specialist ─┼─ race ─► selected result
            └─ cached path ───────┘
```

Race semantics must define:

- what counts as a winner;
- timeout/failure behavior;
- what happens to unfinished losers;
- whether loser results are retained;
- how side effects are treated.

Cancelling a loser is not rollback.

## Real-time observability

Inference and workflow execution should be observable through structured events.

Conceptual events include:

```text
inference.started
prefill.started
prefill.completed
decode.iteration
token.generated
sampling.completed

job.submitted
job.started
job.progress
job.completed
job.failed

workflow.node.started
workflow.node.completed

interrupt.requested
interrupt.applied
inference.paused
inference.resumed
race.winner_selected
```

These events may feed:

- console logs;
- JSONL logs;
- Studio live inspection;
- API/MCP event streams;
- test probes.

Human-readable log strings should not be the only source of truth.

## Trace levels

Observability should be bounded and configurable.

Conceptually:

1. lifecycle/state/timing;
2. token/sampling information;
3. model-exposed reasoning stream where intentionally available and permitted;
4. deep native execution debug probes.

Normal operation must not emit secrets, unrestricted provider payloads, unbounded tensors, or unlimited media/model output.

## Interrupts

Interrupt is a core Runtime feature, not an afterthought.

Possible actions include:

- stop;
- pause;
- resume;
- cancel selected jobs;
- inject bounded input;
- redirect an unexecuted workflow path;
- later, rewind inference state where a native execution profile explicitly supports it.

The Runtime should record both the interrupt request and the point where it actually takes effect.

## Relationship to FLAMORIS AI Agent

A persistent Agent can live naturally above the Runtime.

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
  ├─ active inference state
  ├─ workflow
  ├─ jobs
  ├─ capabilities
  └─ execution events
```

The Runtime may execute reasoning through a loaded model, but it does not become the durable identity or long-term memory authority of the Agent.

## Relationship to other FLAMORIS services

The Runtime may call other services through registered capabilities while preserving their domain authority.

Examples:

- Generation MCP retains generation workflow/job/asset authority;
- GPU Node Manager retains host-wide runtime/GPU lifecycle policy;
- Studio/products retain their own document and UI state;
- external MCP/API/AI services remain external authorities.

`flamoris-intelligence-mcp` may expose or route bounded intelligence capabilities, but AI Runtime owns registered native model execution; external runtimes/providers participate only as Workflow capabilities. Final integration should preserve the Runtime's required inference control points.

## Workflow IR

Workflow IR describes dependencies, data flow, and bounded control.

Jobs are the scheduler-visible execution units created from that plan.

This distinction is intentional:

```text
Workflow
  = what depends on what

Job
  = what is actively running/waiting

InferenceJob
  = a stateful model execution job
```

See [Workflow IR](docs/WORKFLOW_IR.md).

## Workflow compilation

Workflow IR is not executed directly by the scheduler.

The intended path is:

```text
Workflow IR
    ↓
Validator
    ↓
Execution Plan Compiler
    ↓
Execution Plan
    ↓
Jobs / Continuations
    ↓
Scheduler
```

The compiler resolves registered capabilities, validates bindings and limits, derives resource requirements and effects, identifies side-effect boundaries, and records statically known suspension sites/continuation policy.

Actual Continuation instances are created only at runtime. Inference may also yield at runtime-defined control points that were not enumerated as concrete Continuations during compilation.

Compilation is not an authorization grant. The Runtime revalidates current capability pins/availability, caller authorization, concrete input scope, budgets and policy at admission and every dispatch, retry and resume. Effect-specific checks apply immediately before adapter handoff. A cached/reused Execution Plan never carries stale permission as executable authority.

This keeps AI-authored or externally supplied Workflow IR separate from the Runtime's executable scheduling contract.

Effect sets are validated rather than treated as arbitrary labels:

- `pure` is exclusive with `read`, `write`, `external`, `destructive`, and `paid`;
- `read` means reading ambient or mutable state beyond declared immutable inputs;
- `destructive` requires `write`;
- `external` and `paid` are orthogonal attributes that may combine with reads/writes;
- unknown, empty, or contradictory effect metadata fails closed rather than defaulting to `pure`.

## Security model

The broad design principle remains:

> **Open-ended composition. Bounded execution.**

The Runtime should validate and constrain:

- registered capabilities;
- caller authorization;
- side effects;
- resource budgets;
- GPU/CPU/device policy;
- concurrency/fan-out;
- network/filesystem/credential boundaries;
- timeout/cancellation;
- event/log volume.

Model output does not grant permission by itself.

Do not add arbitrary shell, unrestricted Python, ambient filesystem/network access, or embedded credentials as shortcuts.

## Implementation language and kernel boundary

The intended implementation language for the Runtime Kernel is **C++**.

C++ is chosen for FLAMORIS-owned model execution, inference control, cache lifetime, scheduling and resource-aware native CPU/OpenCL compute.

Conceptually:

```text
MCP / API / CLI / language bindings
              │
              ▼
        C++ Runtime Kernel
   ┌──────────┼───────────┐
   ▼          ▼           ▼
Inference   Workflow    Scheduler
 Machine     Machine
      \       /
      Continuation
              │
       Resource Manager
```

C++ does **not** mean every capability must run in-process. External AI, MCP, Generation, and other services remain registered external capabilities with their own authority.

The reviewed [Phase B design](docs/PHASE_B_DESIGN.md) selected the C++20 library-first baseline now implemented in Phase C, including FLAMORIS native model execution with CPU reference and OpenCL compute. No stable ABI or language binding is promised.

See [Implementation Strategy](docs/IMPLEMENTATION_STRATEGY.md).

## Existing FLAMORIS LLM foundation

The private `flamoris-net/flamoris-LLM` is a conceptual and test-methodology foundation.

It already explores:

- model runtime boundaries;
- generation loops;
- cache handling;
- independent processor/tokenizer/model identities and compatibility;
- CPU reference execution;
- OpenCL compute;
- deterministic tests.

The goal is not to copy it wholesale.

Phase B uses the inspected foundation as a concept and test-methodology reference. Direct private-source migration requires separate publication and licensing clearance; none is copied in this proposal. The planned Runtime responsibilities include:

- Inference Controller;
- Job Scheduler;
- Workflow Engine;
- Event Bus;
- Interrupt Control;
- Capability System;
- Resource Manager.

## Runtime research foundation

The [backend research ADR](docs/adr/0001-backend-control.md) compares pinned primary-source revisions of:

- `llama.cpp`;
- Hugging Face Transformers;
- vLLM;
- TensorRT-LLM.

Focus on:

- prefill/decode structure;
- KV/cache ownership and lifetime;
- streaming;
- sampler hooks;
- cancellation;
- pause/resume feasibility;
- scheduler/continuous batching;
- state snapshot/rewind feasibility;
- observability;
- how much control is lost behind server-style APIs.

The purpose is comparative design evidence for a FLAMORIS-owned model runtime, not adapter compatibility. Its tokenizer and decoder qualification includes Japanese, emoji and other non-ASCII sequences under a pinned processor/tokenizer identity.

## Design and implementation phases

Work proceeds through three review gates:

| Stage | Deliverable | Status |
| --- | --- | --- |
| Phase A: Architecture Design | Execution/state/resource/authorization/event/failure contracts and acceptance scenarios | Reviewed and merged via PR #4 |
| Phase B: C++ Implementation Design | Runtime research, concept-to-type mapping, ownership, concurrency, native execution contracts and build/test ADRs | Reviewed baseline; see [design index](docs/PHASE_B_DESIGN.md) |
| Phase C: Implementation | Reviewed contracts implemented with deterministic acceptance evidence | Implemented baseline under integration review |

[Design Phases](docs/DESIGN_PHASES.md) defines the gates and document authority.
Historical design documents describe how the implementation was reached; they are
not substitutes for current executable evidence.

## Current status

The repository now contains the **Phase C native Runtime baseline**, not design
documentation only. Current executable evidence is tracked in
[Phase C status](docs/phase-c/STATUS.md).

Implemented and qualified scope includes:

- C++20 Runtime kernel and transport-independent control surfaces;
- Workflow compilation, Job lifecycle, scheduling and bounded resource accounting;
- authorization/effect checks across dispatch, retry and resume boundaries;
- structured event/observation paths;
- native CPU inference with deterministic tiny causal fixture;
- qualified pause/resume, bounded injection and safe-point control for that native profile;
- OpenCL compute parity on the explicitly recorded qualification device;
- CLI validation plus the developer-only native `generate` smoke-test path.

Important boundaries remain explicit: the tiny fixture is not a pretrained
language model, physical LIME GPU qualification is separate, deployed host
enforcement is separate, and universal model-family/offload/snapshot/rewind
support is not claimed.

See:

- [Phase C implementation evidence](docs/phase-c/STATUS.md)
- [Build and validation](docs/BUILD.md)
- [Native qualification](fixtures/native/QUALIFICATION.md)
- [Runtime Concept](docs/CONCEPT.md)
- [Architecture](docs/ARCHITECTURE.md)
- [Workflow IR](docs/WORKFLOW_IR.md)
- [MCP Contract](docs/MCP_CONTRACT.md)
- [Design Phases and document map](docs/DESIGN_PHASES.md)

## FLAMORIS

FLAMORIS is open-source software for creative work and AI-native production.

Use it however you like.

Commercial use is welcome and does not require permission. If you'd like, we'd be happy to hear what you used FLAMORIS for. This is completely optional.

FLAMORIS software is provided as-is. We do not provide individual support or guaranteed assistance.

If FLAMORIS helps you or you find it interesting, your support helps fund development and keeps the project growing. 🌱

<sub>Mostly GPU bills.</sub>

---

## 日本語

FLAMORIS AI Runtimeは、**モデルを包み、推論とWorkflowを同じ実行ループで制御するAI Runtime**を目指します。

単なる「LLMの外側に置くWorkflow Orchestrator」ではありません。

```text
モデル
  ↓
FLAMORIS AI Runtime
  ├─ 推論
  ├─ Workflow
  ├─ Job Scheduler
  ├─ 割り込み
  ├─ Event / Trace
  └─ Capability
```

という位置です。

開発は **Phase A：アーキテクチャ設計 → Phase B：C++実装設計 → Phase C：実装** の3段階で進めています。現在は **Phase Cのnative Runtime baselineが実装済みで、integration review中** です。C++20のRuntime kernel、Workflow/Job実行、structured event、native CPU推論、限定されたOpenCL parity、pause/resume・bounded injectionなどの実行経路と決定的テストがあります。現在の実装根拠は[Phase C implementation evidence](docs/phase-c/STATUS.md)を参照してください。

推論中に別の処理が必要になれば、Inferenceをyield/pauseし、Vision、algorithm、MCP、外部AIなどをJobとして実行し、その結果を受け取って推論を続けられる構造を目指します。

Jobは並行に実行可能で、Workflowでは `await`、`join`、`race` などを扱えるようにします。

ただし「並行に実行可能」と「GPU上で同時実行する」は同じ意味ではありません。VRAM、model residency、GPU/CPU、外部rate limitなどをSchedulerが見て、実際の実行順序を決めます。

推論過程はstructured eventとしてリアルタイム観測できる設計にします。将来Studioなどから、token、推論stage、active job、queue、race、interrupt、resource状態を見られることを想定します。

途中割り込みもRuntimeの中心機能です。

```text
推論
 ↓
割り込み要求
 ↓
安全なcontrol pointで適用
 ↓
pause / stop / child job / input injection
 ↓
必要ならresume
```

という形を想定します。

Phase Bでは `llama.cpp`、Hugging Face Transformers、vLLM、TensorRT-LLMの一次資料を比較し、prefill/decode、KV cache、streaming、cancel、scheduler、pause/resume、state rewind、observabilityの制御点と制約を記録しました。その設計をもとにPhase CでFLAMORIS native CPU referenceとOpenCL compute、Runtime制御経路を実装しています。外部Runtimeは必要ならWorkflow capabilityとして利用します。

既存の `flamoris-net/flamoris-LLM` も現行コードを調査しました。概念・テスト方針を参考にし、privateコードは移植していません。コードの再利用には公開権限とライセンスの確認が別途必要です。

外側の `flamoris-ai-agent` はIdentity、Memory、Conversation、Personalityなどの永続的なAgent状態を担当し、AI Runtimeは実際にモデルと処理を動かす実行層を担当する想定です。

### 現在試せるCLI

native fixtureを有効にしてビルドすると、自己生成のtiny modelを使った開発用smoke testを実行できます。

```sh
./build/gcc-debug/flamoris-runtime generate \
  --prompt "こんにちは" \
  --temperature 0 \
  --seed 1 \
  --max-tokens 16
```

出力はJSONLで、`token.generated` と `generation.completed` を観測できます。
これは本番model serverやhost authorityではなく、Runtime内部のnative inferenceを
外から確認するための明示的な開発用経路です。

**推論とWorkflowを分離して外から往復させるのではなく、ひとつのRuntimeで握る。**

ここがFLAMORIS AI Runtimeの中心構想です。🐈⚙️

## License

Code and documentation in this repository are licensed under the [Apache License 2.0](LICENSE), unless otherwise noted.

AI models, model weights, datasets, media, and other non-code assets may use separate licenses. State their applicable licenses alongside those assets.
