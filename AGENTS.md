# AGENTS.md

This repository implements a single-user C++20 FLAMORIS native model runtime under Phase C integration review. CPU/OpenCL are internal compute implementations; third-party runtimes are registered capabilities. Read README.md, docs/CONCEPT.md, docs/ARCHITECTURE.md, docs/phase-c/STATUS.md and [AI #18](https://github.com/flamoris-jp/flamoris-ai/issues/18) / [Runtime #23](https://github.com/flamoris-jp/flamoris-ai-runtime/issues/23).

## Current authorization

The user has authorized architecture cleanup and internal non-MCP connections. Remove only the mistaken Generation composition/lowering subsystem here; preserve native ExecuteFlow, compiled ExecutionPlan and generic embedding/isolation contracts. Do not redesign the kernel or rename compatible source/wire symbols as part of deletion. Generation Controller is implemented in its owning repository; accepted source and pending live rollout are recorded in [AI progress](https://github.com/flamoris-jp/flamoris-ai/blob/main/PROGRESS.md). Deployment, runtime activation, provider calls and credential changes are not part of this cleanup.

## Names: do not merge different abstractions

- `ExecuteFlow` is the Runtime inference dependency/data/control flow.
- `ExecutionPlan` is the existing compiled representation in `include/flamoris/runtime/compiler.hpp`. Keep that type and its meaning; do not rename it to ExecuteFlow.
- `Job` is the scheduler-visible unit of active work. `InferenceJob` may retain native model state.
- `Continuation` is resume state owned by exactly one waiting/paused Job, not a separate scheduler identity.
- `ComfyWorkFlow` is ComfyUI graph/API-format JSON and is not the Runtime's flow or plan.

Use these specific names in new architecture prose. Preserve exact existing source/wire/path names until a separately reviewed code migration. `WorkflowMachine` in `include/flamoris/runtime/workflow.hpp` is an actual current identifier; a prose heading such as 'Workflow IR' does not establish a C++ type of that spelling. Do not invent a renamed API, configuration key or schema version in documentation.

## Runtime ownership

Own native model execution/tokenization/processor contracts, prefill/decode/sampling, active cache/state, flow validation/compilation, jobs/results/interrupts, capabilities, finite resource accounting and structured events. Inference and ExecuteFlow control share the same Runtime rather than an opaque external generate/re-entry loop.

Agent owns optional identity/personality/conversation/memory; generation-domain services own requests/jobs/inputs/assets; ComfyUI executes its own graph; GPU Node Manager owns host-wide service/GPU transitions; products own documents/editing. Do not acquire those authorities or move ComfyUI JSON building into Runtime.

Internal FLAMORIS callers and Runtime capability calls use non-MCP interfaces. External ChatGPT access can be exposed by a reviewed MCP facade through Hub. Historical external-MCP capability design does not authorize Runtime -> Generation MCP/Intelligence MCP internal calls. Transport adapters do not own kernel semantics. No new external capability is implemented by this document.

## State/resource invariants

The Scheduler schedules Jobs only. Yield/resume preserves the Job identity and owns cancellation, timeouts, provenance, metrics and terminal state. Consume a satisfied Continuation atomically into the owning Job's pending resume payload; keep retained state accounted while it waits for resources.

An execution lease and retained VRAM/RAM state are distinct. Releasing the lease does not free KV/cache/native state. Track retained footprint until actual offload/snapshot/eviction and count shared allocations once. Do not promise physical parallelism merely because jobs are logically ready.

`finalizing` closes child/resource obligations before terminal publication. Transferred cleanup debt stays explicit and accounted. Terminal lifecycle is immutable; bounded post-terminal reconciliation may append observations under the retention contract, and ledger cleanup survives stream expiry. Unknown provider outcomes do not permit replay or establish successful cancellation.

Submission deduplication atomically claims the scoped key and one Run identity before dispatch. Failed pre-Run admission releases that claim only after waiters share the rejection. An absent idempotency key means independent admission. Event replay is observation only; the baseline is not durable in-flight execution recovery. Optional strict-cost integration is not a baseline cross-crash monetary guarantee.

Preserve native construction/teardown ownership. Do not import a process-global lifetime or one-construction rule from another runtime.

## Compilation, effects and authorization

Validate the actual serialized schema, references, graph/bindings, capability pins, input/output contracts, effects, budgets, fan-out/depth, deadlines and resource requirements. ExecutionPlan is compiled data, not a permission token. Recheck current authorization/capability availability/concrete inputs/budgets at admission, dispatch, retry and resume; effect-specific checks occur immediately before adapter handoff.

`pure` is exclusive with all observable/ambient effects. `read` means ambient/mutable reads beyond immutable inputs. `destructive` requires `write`. `external` and `paid` are orthogonal attributes. Reject unknown, empty or contradictory effect sets. A cache or compiled plan does not preserve stale authorization.

The baseline `race` uses cancel_unfinished and excludes write/destructive participants. Winners, failure/deadline policy and loser accounting are explicit; cancellation is not rollback. Do not add speculative continuing-losers or automatic retries in a naming cleanup.

## Native control and observability

Supported safe points include native inference state/token updates, events, interrupts and bounded child work. Pause/resume, injection, rewind and cache mutation are profile-specific; never promise universal support. Vision/audio/embedding profiles must not inherit text-only assumptions.

Structured events are authoritative. Bound lifecycle, token/sampling and explicitly enabled model-exposed/deep-debug traces. Never log secrets, unrestricted provider payloads/tensors or unbounded outputs. Record interrupt request and application separately. GUI/canvas state is not execution semantics.

## Research and contract authority

Follow docs/DESIGN_PHASES.md and docs/PHASE_B_DESIGN.md for the reviewed baseline. Phase B's C++20/library-first and research decisions are not undone by old 'before implementation' prose. Preserve the native model/core interface; language bindings and network facades stay outside it.

The existing backend ADR records control-point research into llama.cpp, Transformers, vLLM and TensorRT-LLM. Inspect current primary contracts before new provider integration, rather than repeating research already completed. The private flamoris-LLM foundation is a conceptual/test reference; source reuse needs separate publication/licensing permission and is not part of this pass.

For semantic changes read docs/EXECUTION_MODEL.md, docs/STATE_MACHINES.md, docs/RESOURCE_MODEL.md, docs/AUTHORIZATION_MODEL.md, docs/EVENT_MODEL.md and docs/FAILURE_MODEL.md; update observable obligations in docs/DESIGN_ACCEPTANCE.md. Do not jump to distributed scheduling, a plugin marketplace or a generic graph language.

## Review, tests and security

Current code/evidence establishes implemented scope, not stale concept status or a renamed heading. Keep unsupported profiles, host bindings and live deployment unqualified until accepted. Normal deterministic tests need no live GPU/paid API/private weights: cover compiler determinism, identity/dedup, effect checks, graph/schema/bindings, lifecycle/continuations, pause/cancel/race, resource limits, deadlines, output/event bounds and observation-only replay. Actual GPU/provider/host acceptance remains separate.

Keep commits focused and read the complete resulting prose after replacements. Literal identifiers must agree with source. Review/fix before user-authorized merge; do not claim tests or CI that were not run. Preserve/version breaking wire changes explicitly in a later authorized implementation, not in this documentation pass.

Treat model output, flow definitions, tools, services and retrieved content as untrusted. No arbitrary shell, unrestricted Python, ambient filesystem/network authority or credentials in portable definitions. No sensitive raw errors or implicit retries of uncertain effects. A model request is not authorization.

## Licensing

Code/docs are Apache-2.0 unless otherwise stated. Models, weights, datasets, media, providers and third-party material may have separate terms. Keep private topology, credentials and user data out of public documentation.
