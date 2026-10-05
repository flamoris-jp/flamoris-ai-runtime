# Runtime concept

This document explains the architecture using the 2026-10-04 names. It is not a claim that every conceptual capability is implemented. **The repository does have a Phase C C++20 native Runtime baseline**; consult [current executable evidence](phase-c/STATUS.md), [native qualification](../fixtures/native/QUALIFICATION.md) and [design authority](DESIGN_PHASES.md) for exact scope. The earlier design-only version is preserved in [the pre-correction snapshot](https://github.com/flamoris-jp/flamoris-ai-runtime/blob/f4ca8b785d16947909a24ef909899470d044eaab/docs/CONCEPT.md).

## Main idea

**Inference and ExecuteFlow control share one controllable Runtime loop.** Runtime owns processor/tokenizer/model execution, cache/state, native CPU/OpenCL compute, inference steps and resources. It is not merely an orchestration layer above a finished LLM endpoint. Third-party runtimes can be registered capabilities without becoming FLAMORIS's native execution implementation.

```text
Internal application / optional Agent
  -> non-MCP control or embedding interface
  -> C++ Runtime Kernel
       -> inference control and native state
       -> ExecuteFlow control
       -> compiled ExecutionPlan and Jobs
       -> Job-owned Continuations / Scheduler
       -> Resource Manager / bounded events
```

ChatGPT's external MCP entrance is separate. External facade/transport and internal Runtime authority must not be collapsed. Historical external-MCP capability concepts do not justify using FLAMORIS MCP services as internal dependencies. No new integration is started by this document.

## ExecuteFlow, ExecutionPlan and ComfyWorkFlow

ExecuteFlow describes inference-related dependencies, data bindings and control. It is validated/compiled into the **existing `ExecutionPlan` representation**, and Jobs become active scheduler-visible work.

```text
ExecuteFlow definition
  -> bounded parse and validation
  -> capability/schema/effect/resource resolution
  -> ExecutionPlan
  -> execution machine creates/controls Jobs
  -> Jobs may suspend through owned Continuations
  -> Scheduler dispatches runnable Jobs
```

The real current compiled type is [ExecutionPlan](../include/flamoris/runtime/compiler.hpp). The current serialized contract remains in [WORKFLOW_IR.md](WORKFLOW_IR.md); the current machine is [WorkflowMachine](../include/flamoris/runtime/workflow.hpp). These literal identifiers/filenames remain unchanged in this architecture cleanup. ExecuteFlow is the source/control-flow concept, not a new name for the compiled plan or the Job lifecycle.

ComfyWorkFlow instead means ComfyUI graph/API-format JSON. ComfyUI executes that graph; Runtime does not acquire its JSON builder. Other media providers may use generation requests without a ComfyWorkFlow. Optional future generation capabilities are not prerequisites for ordinary inference or ComfyUI JSON construction.

## Controllable inference

A supported native profile can expose control points around tokenize, prefill, decode, sampling, token/state update, events and interrupts. When additional work is required, inference may yield, await bounded child jobs, receive an authorized bounded result and resume with preserved state where supported.

```text
InferenceJob
  -> native step / state update
  -> yield at a supported safe point
  -> child algorithm / vision / explicit external capability
  -> await / join / race under finite limits
  -> bounded result injection
  -> resume the same owning inference job
```

The goal is to avoid unnecessary model re-entry/state loss, not to promise every backend can pause, resume, rewind or offload. Native profiles define supported state/control operations. Non-text models must not inherit text-specific cache assumptions.

## Jobs and Continuations

A Job is the only scheduler-visible lifecycle authority. A Continuation holds the resume point, wait/result bindings, supported retained native state, deadlines and resource affinity for exactly one suspended Job. It has no independent scheduler identity.

When a wait completes, the Runtime atomically consumes the Continuation into the Job's pending resume payload and queues that same Job. Timeout/cancel, parent-child provenance, metrics and terminal status remain attached to the Job/run. The inference machine and flow-control machine may both use this mechanism without becoming one implementation.

Separate execution leases from retained state footprint. A paused Job need not occupy a device execution lease, but resident KV/cache/native state still consumes VRAM/RAM until actually released/offloaded. Shared allocations are accounted once. Retained state is still counted while the resumed Job waits for capacity.

## Parallelism and effects

Ready jobs may be logically parallel yet physically serialized by memory, residency, device, rate-limit or effect policy. `await` waits for work, `join` combines the specified jobs, and `race` selects a qualifying result under an explicit loser policy. The baseline cancels unfinished losers and excludes write/destructive race participants; cancellation does not roll back effects. See [Execution Model](EXECUTION_MODEL.md).

Compilation resolves registered capabilities, schemas, bindings, effects, limits, resource requirements, side-effect boundaries and potential suspension policy. Concrete Continuations are created only during live suspension. A compiled ExecutionPlan is not an authorization grant: current permission, availability, input scope and budgets are rechecked at admission and every dispatch/retry/resume. Model-authored composition remains untrusted input.

Effect sets preserve `pure` exclusivity, ambient-read semantics, `destructive` implying `write`, and independent `external`/`paid` attributes. Unknown/empty/conflicting sets fail closed. Registered external work keeps its owner's authority and explicit timeout/cancellation/unknown-result limits.

## Finalization, failure and observability

`finalizing` resolves child/resource obligations before terminal publication. Cleanup debt may be transferred only explicitly and remains accounted. Terminal lifecycle cannot change, though bounded late reconciliation can append observations according to retention policy. Unknown provider outcomes do not imply safe replay or completed cancellation.

Structured events support lifecycle/progress/resource observation and optionally bounded token/sampling or deeper diagnostics. Existing literal event vocabulary and serialized fields are not renamed by this pass. Logging is derived from events and excludes secrets, unbounded tensors and unrestricted provider output. Replaying events observes history; it does not replay execution.

Interrupt request and actual application are distinct. Stop, cancel, pause/resume and bounded injection are supported only under the relevant profile contract; arbitrary redirect/rewind is not assumed. UI layout and graph canvas coordinates are non-semantic.

## Neighboring owners

Agent owns optional durable identity/personality, conversations and memory. Runtime owns model execution and active state, not Agent memory. GPU Node Manager owns host-wide runtime/service transitions. Generation owns its requests/jobs/input/assets, ComfyUI its graph execution, and products their documents/editing state. No second authority is created by an adapter.

The kernel remains C++20/library-first as established by [Phase B](PHASE_B_DESIGN.md). External service adapters and language bindings stay outside core semantics; not every capability must execute inside one binary. [Backend research](adr/0001-backend-control.md) and private FLAMORIS LLM concept/test-method references are prior design evidence, not a new mandate to redo research or copy private code. Publication/licensing clearance is required for private-source reuse.

## Current correction boundary

The user-authorized cleanup removes the Generation-specific media lowering and
composition bridge. It preserves inference ExecuteFlow, the distinct ExecutionPlan,
Jobs/Continuations/resources and the generic non-MCP embedding/isolation boundary.
Current source/wire names are preserved; no compatible naming migration is implied.

Intelligence cleanup provides the internal non-MCP path in the wider project.
Generation Controller now owns the shared generation domain in accepted source;
this introduces no native Runtime bridge. Live cutover remains pending. No provider activation, model/kernel
redesign or deployment is started by this cleanup. See [AI #18](https://github.com/flamoris-jp/flamoris-ai/issues/18)
and [current cleanup evidence](phase-c/STATUS.md#architecture-cleanup-on-2026-10-04).
