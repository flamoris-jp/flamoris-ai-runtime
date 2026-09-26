# Implementation Strategy

## Status

**Design decision. Runtime implementation has not started yet.**

This document records the intended implementation boundary for FLAMORIS AI Runtime.

## Core decision

The intended implementation language for the **Runtime Kernel is C++**.

The Runtime is model-adjacent rather than only an orchestration service. It is expected to own or closely coordinate inference lifecycle control, backend/model state, cache lifetime, continuation state, scheduling, resource decisions, interrupts, and structured events.

C++ is therefore the default language for the execution kernel.

This decision does not require every capability or FLAMORIS service to be implemented in C++.

## Intended process boundary

```text
       Agent / ChatGPT / Studio / other callers
                         │
                MCP / API / CLI
                 optional bindings
                         │
                         ▼
                C++ Runtime Kernel
        ┌────────────────┼────────────────┐
        ▼                ▼                ▼
Inference Machine   Workflow Machine   Event Journal
        │                │
        └──────────┬─────┘
                   ▼
           Job (scheduler-visible)
                   │
      waiting/paused owns optional
                   ▼
             Continuation
              resume state

Scheduler schedules Jobs only.
Resource Manager accounts execution leases and retained state footprints.
                                   │
               ┌───────────────────┼───────────────────┐
               ▼                   ▼                   ▼
          native model        local capability    external capability
          backend/runtime                         MCP/API/service
```

The Runtime Kernel remains headless and transport-independent.

MCP, HTTP/API, CLI, and future language bindings are adapters over Runtime contracts. They must not become the place where inference/workflow semantics live.

## Why C++

The design requires unusually direct execution control compared with a normal workflow service.

Expected needs include:

- explicit prefill/decode/sampling control where supported;
- ownership or close coordination of model/backend state;
- cache/state lifetime management;
- low-latency interrupt points;
- predictable Job and Continuation lifecycle;
- resource-aware scheduling;
- model residency and switching-cost decisions;
- native integration with low-level model runtimes and compute backends;
- deterministic offline lifecycle tests;
- embeddability without requiring a Python interpreter or another orchestration runtime.

C++ fits this kernel role while keeping external capabilities free to use the implementation language appropriate to their own domain.

## C++ is not an in-process mandate

External services retain their existing authority.

Examples:

- Generation remains a registered external capability/service;
- MCP tools remain external authority boundaries;
- remote AI/API providers remain external;
- GPU Node Manager retains host-wide GPU/runtime lifecycle authority;
- Agent identity and durable memory remain outside this Runtime.

The C++ kernel coordinates execution. It does not absorb unrelated systems.

## Internal architectural boundaries

The first implementation should preserve explicit modules/contracts for:

- `runtime` - run lifecycle and top-level coordination;
- `inference` - Inference Machine and backend-facing lifecycle;
- `workflow` - Workflow IR validation, Execution Plan Compiler, Workflow Machine;
- `continuation` - resumable state and resume conditions;
- `jobs` - scheduler-visible work lifecycle;
- `scheduler` - readiness, priority, resource-aware placement;
- `resources` - CPU/RAM/GPU/VRAM/model-residency view;
- `capabilities` - registered callable operations and effect metadata;
- `events` - structured event schema, journal, trace replay;
- `adapters` - MCP/API/CLI and optional language bindings;
- `backends` - model/runtime-specific integration.

These are conceptual boundaries, not frozen directory names.

## Ownership and lifetime

C++ implementation should make ownership explicit.

Long-lived objects such as model/backend handles, run state, Continuations, and event journals must have clear lifetime authority.

Do not rely on ambient global mutable state for:

- active runs;
- backend/model ownership;
- GPU leases;
- Continuations;
- capability registry;
- event subscribers;
- credentials/configuration.

The exact smart-pointer/handle strategy is not frozen yet.

## Continuation and resource ownership

A Continuation represents resumable state owned by exactly one waiting/paused Job. It has no independent scheduler identity; the Job remains the sole authority for cancellation, timeout, provenance, metrics, and terminal state.

It may retain an approved reference to model/backend state where the backend supports preservation, but it should not normally retain a physical **execution lease** while waiting. Releasing that lease does not imply that backend state was freed.

```text
yield
  ↓
Continuation
  ├─ resume point
  ├─ state reference
  ├─ waiting condition
  ├─ bindings
  ├─ resource affinity
  └─ retained state footprint
  ↓
wait without pinning the execution lease
  ↓
Resource Manager still accounts resident VRAM/RAM state
  ↓
scheduler reacquires execution capacity
  ↓
same owning Job resumes
```

This allows LIME and future nodes to use scarce GPU/VRAM resources across different jobs without losing useful warm-model affinity. A retained footprint remains allocated until state is actually offloaded, snapshotted/migrated, or evicted; lease release alone is not treated as free memory.

## Workflow compilation boundary

Portable Workflow IR is not executed directly.

```text
Workflow IR
  ↓
Validator
  ↓
Execution Plan Compiler
  ↓
Execution Plan
  ↓
Workflow Machine / Jobs / Continuations
  ↓
Scheduler
```

The compiler is the execution-normalization and static-analysis boundary between declarative composition and executable runtime state. It may calculate required permissions/effects/resources, but it does not grant durable authorization.

Current capability availability, caller authorization, budgets, and policy are revalidated at run admission. Side-effecting dispatch is revalidated immediately before execution when required by policy. Cached/reused plans never act as permission tokens.

The plan contains only statically known potential suspension sites and continuation policy. Concrete Continuation instances are created at runtime when the owning Job actually yields.

## Effects

Capability effects are represented as a set rather than one boolean.

Initial conceptual effects:

- `pure`
- `read`
- `write`
- `external`
- `destructive`
- `paid`

Initial invariants:

- every effect set is non-empty and contains only known values;
- `pure` is exclusive with every other effect;
- `read` means ambient/mutable-state read beyond declared immutable inputs;
- `read` and `write` may coexist;
- `destructive` requires `write`;
- `external` and `paid` are orthogonal attributes that may combine with non-pure effects;
- invalid/unknown combinations fail closed.

The compiler derives effect summaries and side-effect boundaries before execution.

This supports authorization, confirmation, retry/idempotency rules, race restrictions, provenance, and budgets.

## Event journal and replay

Structured events are the observable source of truth.

The Runtime may persist a bounded event journal for inspection and regression testing.

Keep these operations distinct:

- trace replay: replay recorded events only;
- retry/re-execution: execute work again under current authorization/effect rules.

Trace replay must never perform side effects.

## Model residency

Resource management should eventually treat model residency as a first-class scheduling input.

Useful state may include:

- logical model/backend identity;
- current device;
- approximate resident model memory;
- retained per-Job/Continuation state footprint;
- active execution leases;
- warm/cold state;
- estimated eviction/reload/offload cost.

A scheduler may reorder otherwise-ready work to reduce switching cost only when doing so preserves dependency, effect, fairness, budget, cancellation, and priority semantics.

## Race and speculative execution

The first race implementation should stay deterministic and small:

- explicit participants;
- explicit acceptance rule;
- timeout/failure semantics;
- explicit loser policy.

Later speculative execution may allow provisional results followed by stronger final results.

That later feature requires explicit result/event states such as:

- `provisional`;
- `final`;
- `superseded`.

Do not overload ordinary race semantics with UI replacement behavior in the first implementation.

## Decisions intentionally deferred to Phase 0

Do not freeze these from design conversation alone:

- minimum C++ language standard;
- supported compilers/platform matrix;
- exact CMake/build/package layout;
- exception versus result/error strategy;
- coroutine/threading model;
- allocator strategy;
- stable C ABI;
- Python/C#/other bindings;
- plugin ABI;
- exact backend interface;
- exact GPU backend support.

Choose them after reviewing:

- current `flamoris-net/flamoris-LLM`;
- `llama.cpp`;
- Hugging Face Transformers runtime/cache behavior;
- vLLM;
- TensorRT-LLM;
- the actual LIME deployment constraints.

## Initial implementation order

1. runtime/backend research and decision records;
2. inspect and extract reusable `flamoris-LLM` model/inference pieces;
3. establish the C++ Runtime Kernel skeleton and structured Event contract;
4. implement Inference Machine lifecycle;
5. implement Continuation lifecycle;
6. implement Job lifecycle and simple Scheduler;
7. implement Workflow IR Validator and Execution Plan Compiler;
8. implement Workflow Machine over compiled plans;
9. implement interrupt/cancel and supported pause/resume;
10. add MCP/API/CLI adapters;
11. add real capabilities;
12. optimize event replay, residency-aware scheduling, and speculative execution after measured need.

The first milestone should prove execution semantics, not maximize features.
