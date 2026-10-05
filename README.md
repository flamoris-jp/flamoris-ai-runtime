# FLAMORIS AI Runtime

**Inference and ExecuteFlow, controlled in one runtime.**

FLAMORIS AI Runtime is a single-user native model runtime implemented in C++20. It owns model execution, cache/state and inference control together with execution-flow control, jobs, interrupts, resource accounting and observability. CPU reference and OpenCL compute are internal implementations; third-party runtimes participate as registered capabilities, not as the native model backend.

**Status: Phase C baseline implemented and under integration review.** See [implementation evidence](docs/phase-c/STATUS.md), [build instructions](docs/BUILD.md) and [native qualification](fixtures/native/QUALIFICATION.md). The self-authored tiny causal fixture is not a pretrained language model or proof of general model-family support. OpenCL qualification used a POCL CPU device; physical GPU and deployed host integration remain separate evidence.

Part of the [FLAMORIS AI ecosystem](https://github.com/flamoris-jp/flamoris-ai/blob/main/docs/ai-ecosystem.md).

## Terminology and correction scope

[AI #18](https://github.com/flamoris-jp/flamoris-ai/issues/18) and [Runtime #23](https://github.com/flamoris-jp/flamoris-ai-runtime/issues/23) define the corrected names:

| Name | Meaning |
| --- | --- |
| `ExecuteFlow` | Runtime inference dependency/data/control flow |
| `ExecutionPlan` | Existing compiled Runtime representation; retain this type and its distinct meaning |
| `ComfyWorkFlow` | ComfyUI execution graph / API-format JSON; not owned by AI Runtime |

The actual compiled C++ type is `ExecutionPlan` in [compiler.hpp](include/flamoris/runtime/compiler.hpp). The current execution machine is `WorkflowMachine` in [workflow.hpp](include/flamoris/runtime/workflow.hpp). The document [WORKFLOW_IR.md](docs/WORKFLOW_IR.md) describes the current serialized input contract. These literal code/path names are preserved by this cleanup. Do not invent a C++ `WorkflowIR` type from a prose heading or rename the compiled plan to ExecuteFlow.

New architecture prose uses the specific names rather than bare Workflow. Exact current wire fields, schema revisions, filenames, external names and historical quotations retain their spelling until a separately reviewed compatibility change.

The architecture cleanup removes the mistaken Generation composition/lowering subsystem while preserving native inference and ExecuteFlow. Generic non-MCP embedding, pinned admission and principal isolation remain; see [cleanup evidence](docs/phase-c/STATUS.md#architecture-cleanup-on-2026-10-04). Generation Controller is implemented in its owning repository; accepted source and pending live rollout are recorded in [AI progress](https://github.com/flamoris-jp/flamoris-ai/blob/main/PROGRESS.md). This source change does not deploy services or invoke providers.

## Build and existing CLI

These are current development commands, not authorization to run production inference:

```sh
cmake --preset gcc-debug -DFLAMORIS_NATIVE_TESTS=ON
cmake --build --preset gcc-debug --parallel 2
ctest --preset gcc-debug --no-tests=error
```

Current CLI entries:

```text
flamoris-runtime validate FILE
flamoris-runtime serve
flamoris-runtime generate --prompt TEXT [--temperature FLOAT] [--seed UINT] [--max-tokens UINT]
```

`validate` checks/compiles a bounded submission and prints its fingerprint. `serve` exposes the control protocol but provides no local host authority by itself, so it remains fail-closed until a trusted embedding supplies capacity.

`generate` is a developer-only smoke path for the native tiny fixture, using the CPU `NativeSession` and JSONL `token.generated` / `generation.completed` events. It is not a production model server or a host-capacity grant.

```sh
./build/gcc-debug/flamoris-runtime generate \
  --prompt "Hello 日本語" \
  --temperature 0 \
  --seed 1 \
  --max-tokens 16
```

The default fixture is `fixtures/native/tiny-causal-v1.bin`; `--model FILE` accepts only the same registered fixture format/checksum contract. Setup/build/test details remain in [BUILD.md](docs/BUILD.md).

## Model-adjacent execution

Runtime is not merely an outer loop over an opaque model endpoint. Its control points include tokenize/prefill/decode/sampling, state/cache ownership, events, interruption and bounded child work where the qualified native profile supports them.

```text
native inference step
  -> update tokens/state and emit events
  -> apply supported interrupt / yield
  -> run or await bounded child jobs
  -> inject an authorized bounded result
  -> resume the same inference job where supported
```

Do not infer universal pause/resume, rewind, offload or snapshot support. Retained model state and control guarantees are native-profile capabilities with explicit qualification. Vision, audio and other profiles must not inherit text-only state assumptions.

## ExecuteFlow, compiled plan and jobs

```text
ExecuteFlow definition (current serialized contract)
  -> validation and compilation
  -> ExecutionPlan (existing compiled representation)
  -> execution machine / Jobs / Job-owned Continuations
  -> Scheduler
```

The compiler resolves capability pins, schemas, bindings, effects, limits, resources and static suspension policy. Concrete Continuations exist only when a Job actually suspends. An AI-authored input definition is untrusted; compilation is not authorization. Revalidate current capability availability, caller scope, inputs, effects and budgets at admission and every dispatch, retry and resume.

A Job is the only scheduler-visible lifecycle authority. A Continuation is resume state owned by one waiting/paused Job, not a second scheduled identity. When its wait is satisfied, consume it atomically into that Job's pending resume payload and queue the same Job. Cancellation, timeout, provenance, metrics and terminal state stay with the Job/run. See [State Machines](docs/STATE_MACHINES.md).

A paused Job should not pin an execution lease indefinitely. Releasing that lease does not prove KV/cache/native state left VRAM/RAM. Account retained state until it is actually offloaded, snapshotted elsewhere or evicted; count shared allocations only once.

Logical parallelism does not promise physical simultaneous GPU execution. Resource, residency and effect policy may serialize ready jobs. `await`, `join` and `race` retain bounded semantics; the baseline race policy cancels unfinished losers and excludes write/destructive participants. Cancellation is not rollback. See [Execution Model](docs/EXECUTION_MODEL.md).

## Security and observable state

Effects are validated: `pure` is exclusive with other effects, `read` concerns ambient/mutable state beyond immutable inputs, `destructive` requires `write`, and `external`/`paid` are orthogonal. Reject unknown, empty or contradictory sets.

Structured events are the observability authority, not free-form log text. Keep bounded lifecycle, token/sampling and explicitly permitted deeper traces; do not log secrets, unrestricted tensors or provider payloads. Event replay is observation, not side-effect execution recovery. Existing literal event names such as `workflow.node.started` are historical/protocol vocabulary, not renamed or newly promised by this documentation.

Record an interrupt request separately from the point at which it applies. `finalizing` closes child/resource obligations before terminal publication; any transferred cleanup debt remains explicit and accounted. Terminal lifecycle is immutable, while separately bounded post-terminal reconciliation may append observations under the existing retention contract. An unknown remote outcome is neither successful cancellation nor permission to replay.

Do not add arbitrary shell, unrestricted Python, ambient network/filesystem authority or credentials inside portable definitions. Model output alone grants no permission. Strict cross-crash monetary limits remain an optional integration, not a baseline guarantee.

## Internal and external boundaries

Internal applications use Runtime's non-MCP interfaces. Agent is optional personality/conversation/memory above execution, not a required step. External ChatGPT access may use a reviewed MCP facade through Hub. Runtime must not call FLAMORIS's Generation MCP or Intelligence MCP as an internal service bus. Earlier third-party external-MCP capability ideas do not override this boundary or authorize new integrations.

Generation-domain services own their requests/jobs/inputs/assets; ComfyUI executes ComfyWorkFlow. Future generation capability integration is optional, not a prerequisite for Runtime or simple ComfyUI JSON building. GPU Node Manager retains host-wide runtime/GPU lifecycle; Runtime's resource accounting does not duplicate that state machine. Products retain documents/editing state.

## Design evidence and document map

Phase A established architecture contracts; Phase B established the C++20/library-first implementation design; Phase C contains executable evidence. Historical concepts do not require redoing completed design/research. No stable ABI or language binding is promised.

- [Runtime concept](docs/CONCEPT.md) and [architecture](docs/ARCHITECTURE.md)
- [Current input format](docs/WORKFLOW_IR.md)
- [Design phases](docs/DESIGN_PHASES.md), [Phase B index](docs/PHASE_B_DESIGN.md) and [implementation strategy](docs/IMPLEMENTATION_STRATEGY.md)
- [Backend research ADR](docs/adr/0001-backend-control.md)
- [MCP design reference](docs/MCP_CONTRACT.md), not an internal dependency mandate

The backend ADR records pinned research into llama.cpp, Transformers, vLLM and TensorRT-LLM control points, not a promise of compatibility. The private FLAMORIS LLM foundation is a conceptual/test-method reference; private source reuse requires explicit publication and licensing clearance. This naming change copies no private source.

## 日本語

Runtimeはモデルに隣接して推論とExecuteFlowを制御するC++20の実行基盤です。ExecuteFlowは依存・データ・制御のフロー、ExecutionPlanは既存のコンパイル済み表現、Jobは実行中の仕事です。名前の変更でこの三者を統合しません。

ComfyWorkFlowはComfyUI用グラフ・JSONで、Runtimeへ移しません。Agentは人格が必要な場合のみ。内部MCP通信を前提にせず、誤って追加したGeneration専用の変換・橋渡しを削除しました。推論のExecuteFlowと既存ExecutionPlanは維持します。Phase Cの実装はありますが、tiny fixtureの検証と実モデル・実GPU・実機導入の受け入れは別です。

## FLAMORIS and license

FLAMORIS is open-source software for creative work and AI-native production. Commercial use of licensed code is welcome without individual permission. It is provided as-is without guaranteed individual support; documentation, Issues, tests and source are primary self-support references.

Code and documentation are [Apache-2.0](LICENSE), unless otherwise noted. Models, weights, datasets, media and other non-code assets may have separate terms.
