# Implementation Strategy

## Status

**Phase A is merged; [Phase B implementation design](PHASE_B_DESIGN.md) is proposed for review. Runtime implementation has not started.**

[Design Phases](DESIGN_PHASES.md) defines the three review gates and supersedes the old Phase 0–3 implementation sequence. This document records the Phase A C++ boundary and required Phase B questions. The linked Phase B proposal supplies the concrete decisions, evidence and acceptance-to-test mapping; no production interface or behavior is implemented.

## Core decision

The Runtime Kernel implementation language is **C++**. Its model-adjacent control, explicit state ownership, resource accounting, scheduling and event contracts remain headless and transport-independent.

MCP/API/CLI and future bindings adapt those contracts. External AI, Generation and product services retain their own process and domain authority; this decision does not require rewriting them in C++.

| Concern | Kernel contract | Outside the kernel |
| --- | --- | --- |
| Execution | Run/Job lifecycle, control ordering and budgets | Transport and caller UI |
| Inference | Capability-honest control and state lifetime | Backend-specific compute implementation |
| Workflow | Validation, compiled plan and bounded composition | AI authoring and visual editing |
| Resources | Runtime allocations, reservations and cleanup | Host-wide authority in GPU Node Manager |
| Data | Active state and bounded result bindings | Agent memory and Generation asset ownership |
| Observation | Structured events and replay semantics | Rendering, optional journal sinks and clients |

## Semantics before types

The normative contracts are [Execution Model](EXECUTION_MODEL.md), [State Machines](STATE_MACHINES.md), [Resource Model](RESOURCE_MODEL.md), [Authorization Model](AUTHORIZATION_MODEL.md), [Event Model](EVENT_MODEL.md), and [Failure Model](FAILURE_MODEL.md).

An implementation must preserve:

- Job as the only scheduler-visible lifecycle identity;
- Continuation owned by a waiting/paused Job, consumed atomically on enqueue into a Job-owned pending resume payload;
- backend state lifetime across queue waits, independently accounted from execution leases;
- immutable Execution Plans without live state, credentials or authorization grants;
- current admission/dispatch/resume authorization and bounded dynamic child envelopes;
- cleanup/finalizing distinct from successful calculation;
- ordered committed events without making subscriber delivery part of execution correctness;
- inspection-only trace replay and no implicit restart recovery.

The initial concurrency architecture is one logical state-transition authority per Run; this is not a decision to run backend compute on one thread. Worker callbacks submit observations to that authority and cannot independently mutate terminal state. Phase B chooses the synchronization/threading mechanism and proves its lifetime rules.

## Concept-to-implementation mapping required in Phase B

Names below describe required responsibilities, not declarations or a frozen ABI.

| Architecture concept | Implementation design must identify |
| --- | --- |
| Run | Identity, owner scope, limits, cancellation tree and terminal aggregation |
| Job | Identity, state record, controller, attempt generation and finalizing intent |
| Continuation | Exclusive owner, resume point, wait condition, validity and pending-resume transfer |
| Execution Plan | Immutable versioned representation, pinned contracts, dependencies and bindings |
| Machines | Inference/backend control versus workflow/dependency control |
| Resource Manager | Allocation/lease/reservation handles, generations and cleanup debt |
| Capability | Registry snapshot, effect set, current availability and adapter dispatch |
| Authorization | Current policy decision, scoped execution input, atomic submission claim and Run identity, and process-local limits |
| Paid Budget Authority | Optional future strict-cost external capability integration; not a baseline dependency |
| Event | Envelope, sequence, bounded payload and journal/subscriber boundary |
| Failure | Typed code, external outcome certainty, retry and cleanup disposition |

No ambient mutable singleton may own active Runs, model state, GPU leases, Continuations, registry, credentials or event subscribers. Ownership transfer and callbacks after cancellation require explicit lifetime proofs.

## Required research

Before freezing long-lived backend/inference interfaces, inspect current `flamoris-net/flamoris-LLM` through the connected GitHub integration. Evaluate model/runtime, generation loop, cache, tokenizer/model, compute, CPU reference, OpenCL and deterministic-test boundaries. Reuse deliberately; do not copy model internals into Workflow/Scheduler layers. Public conclusions must not include private code or environment details.

Compare current primary sources for `llama.cpp`, Hugging Face Transformers, vLLM and TensorRT-LLM. Record revisions and sources in an ADR. The comparison must answer what FLAMORIS must own directly:

- tokenize, prefill, decode and sampler control;
- cache/state ownership, context compatibility and release acknowledgements;
- streaming and cancellation safe points;
- whether pause/resume preserves sampling and backend state;
- offload/snapshot/rewind capabilities and their restrictions;
- batching/scheduling interaction and memory ownership;
- observability and limitations of opaque server APIs.

Phase A does not claim this research has been completed or select a backend. Architecture requirements are not evidence that any particular backend supports them. Deployment constraints used in Phase B must be verified from current authorized sources; old setup notes are historical context.

## Decisions reserved for Phase B

Choose only what the first implementation needs:

- minimum C++ standard and supported toolchain/platform matrix;
- CMake/build/package and source/test layout;
- namespace/module and dependency boundaries;
- result/error versus exception strategy across backend/adapter boundaries;
- RAII/handle/smart-pointer ownership and destruction thread requirements;
- worker model, synchronization, clocks, timers and cancellation tokens;
- event/ID/schema representation and parser/resource bounds, including bounded post-terminal Run observation;
- atomic process-local submission claim/Run admission and duplicate-waiter arbitration;
- optional strict-cost profile may later define durable reservation and reconciliation, without blocking baseline native inference;
- native model/compute ownership and host coordination protocol;
- deterministic fake clocks, backend callbacks and allocation test seams.

Do not prematurely promise a stable public ABI, plugin ABI, language binding, allocator framework, distributed scheduler or coroutine framework. These choices need an implementation requirement and evidence.

## Implementation gate

Phase B produces design documents/ADRs and an acceptance-to-test map, then receives review. Phase C adds build support and production code in meaningful, frequently committed units. See [Design Phases](DESIGN_PHASES.md) for the delivery order and [Design Acceptance](DESIGN_ACCEPTANCE.md) for the behavioral obligations.

Neither merging this proposal nor a successful documentation check means inference, pause/resume, a scheduler or an MCP server exists.
