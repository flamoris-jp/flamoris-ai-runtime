# Design and delivery phases

## Status and scope

**Phase A architecture proposal for review. No runtime code, build system, or tests are implemented.**

This plan replaces the earlier interleaved Phase 0–3 research/implementation roadmap. The three stages are sequential review gates: architecture, C++ implementation design, then implementation. A merged architecture proposal does not claim backend feasibility or authorize skipping the next gate.

The purpose is to settle observable semantics before choosing convenient implementation types. It is not a promise that implementation can never uncover a design defect. Record a deviation with affected invariants, alternatives, migration impact, and new acceptance cases; update and review the design before implementing a semantic change.

## Document authority

| Document | Owns |
| --- | --- |
| [Concept](CONCEPT.md) | Product intent and long-term direction |
| [Architecture](ARCHITECTURE.md) | Component and neighboring-service boundaries |
| [Execution Model](EXECUTION_MODEL.md) | Run/Job ownership, plan execution, bounded composition |
| [State Machines](STATE_MACHINES.md) | Allowed transitions, control ordering, terminal rules |
| [Workflow IR](WORKFLOW_IR.md) | Declarative inputs, compilation and binding semantics |
| [Resource Model](RESOURCE_MODEL.md) | Capacity, allocation lifetime, residency, quarantine |
| [Authorization Model](AUTHORIZATION_MODEL.md) | Effects, current authorization, budget and dispatch checks |
| [Event Model](EVENT_MODEL.md) | Committed event ordering, bounded observation and replay |
| [Failure Model](FAILURE_MODEL.md) | Failure classification, propagation, retry and recovery limits |
| [MCP Contract](MCP_CONTRACT.md) | Transport-facing projection of kernel semantics |
| [Implementation Strategy](IMPLEMENTATION_STRATEGY.md) | C++ boundary and Phase B deliverables |
| [Design Acceptance](DESIGN_ACCEPTANCE.md) | Cross-document scenarios and future test obligations |

Detailed contracts govern their own domain; overview examples are explanatory. If documents conflict, fix the conflict rather than selecting whichever makes implementation easiest. Wire names and C++ types remain unfrozen until Phase B. Phase A semantic changes require review even when the wire schema is still draft.

## Phase A — architecture design

Design the complete execution story, including failure paths, without adding production code or a build scaffold.

In scope:

- Run, Job, Continuation, Inference Machine and Workflow Machine authority;
- immutable compiled plans, dependency/binding rules and bounded dynamic child work;
- interrupt application, cancellation, timeout, pause/resume and join/race;
- resource reservations, retained state, model residency and external host authority;
- capability/effect contracts, authorization, idempotency and budget enforcement;
- structured events, journal retention, trace replay and process failure limitations;
- external AI/MCP, Agent, Generation, products and GPU Node Manager boundaries.

The baseline is a single Runtime process controlling its own Runs and accounting ledger. It may call external services. It is not a distributed scheduler and does not promise restart-safe continuation, exactly-once external effects, universal pause/rewind, or automatic rollback.

Exit gate:

1. Each state and resource has one authority and an explicit lifetime.
2. [Acceptance scenarios](DESIGN_ACCEPTANCE.md) have defined outcomes and event/resource obligations.
3. No unresolved architectural blocker remains after independent review.
4. Unsupported and future behavior is explicit; the repository still says design-only.

## Phase B — C++ implementation design

Begin after Phase A review. Do not add production runtime code at this stage.

Required evidence:

- inspect current `flamoris-net/flamoris-LLM` through authenticated GitHub access;
- compare current primary-source contracts of `llama.cpp`, Transformers, vLLM and TensorRT-LLM;
- record source revisions/date, actual control points, limitations and reuse/licensing decisions;
- choose the first backend only from this evidence; do not assume a server endpoint supports decode-level control or state preservation.

Research must cover tokenization, prefill/decode, sampling, KV/state ownership, streaming, cancellation, pause/resume, offload/snapshot/rewind, batching, observability, and embedded versus server APIs. An unavailable source remains an explicit research blocker, not a guessed implementation description. Private code/topology must not be copied into public documentation.

Required design outputs:

| Area | Reviewable deliverable |
| --- | --- |
| Toolchain | ADR for C++ standard, compilers, build/test/package layout and dependency policy |
| Domain mapping | Concept → C++ type/interface table, ownership/lifetime and invariants |
| Errors | Result/exception boundary, typed failure propagation and cancellation representation |
| Concurrency | State commit/linearization, worker callbacks, thread affinity, clocks and cancellation safety |
| Resources | Handle/allocation ownership, cleanup/quarantine protocol, host-adapter contract |
| Backend | Evidence-backed capability matrix, first integration contract and deterministic fake seams |
| Serialization | Versioned IR, plan/event/error shapes, redaction, bounded parser and compatibility rules |
| Delivery | Implementation slices, exact acceptance case mapping, CI/toolchain matrix and risk register |

A C++ concept table is a design deliverable, not a stable public ABI. Do not freeze plugin ABI, bindings, allocator architecture, distributed execution or a complex coroutine framework without a concrete need. Focused feasibility experiments require a separate explicit scope and must not be presented as implemented product behavior.

Exit gate: all initial implementation interfaces are backed by evidence, ownership and concurrency rules are concrete, all acceptance cases map to deterministic test seams, and there are no unresolved blockers for the first delivery slice. Review Phase B before Phase C.

## Phase C — implementation

Implement the reviewed Phase A/B contracts in logical, frequently committed slices:

1. build/test foundation and domain/error/event types;
2. Job/Continuation lifecycle and deterministic fake backend;
3. resources, simple Scheduler, authorization and budgets;
4. validator/compiler and Workflow Machine;
5. Inference Machine and evidence-selected first backend;
6. registered adapters and transport surfaces;
7. journal/replay and later optimizations after baseline acceptance.

Exact dependency order is a Phase B deliverable. A slice must not advertise a capability until its code and tests exist. Use offline deterministic lifecycle tests before GPU/provider-dependent integration tests. Keep PRs reviewable and do not auto-merge.

## Baseline versus later extensions

| Concern | Baseline contract | Later extension |
| --- | --- | --- |
| Workflow | Bounded DAG, declared child-work envelopes, immutable plans | Revisioned adaptive patching and bounded loops |
| Pause | Only advertised safe points and valid preserved state | Cross-process snapshots and migrations |
| Race | Explicit acceptance, fixed participants and bounded loser cleanup | Provisional/final/superseded result streams |
| Resources | Conservative accounting, fair readiness and safe model reuse | Measured residency/reload optimization |
| Events | Bounded ordered observation; inspection-only replay | Durable execution recovery under a separate protocol |
| Backends | One researched, capability-honest backend | Additional backends, including Vem after a stable contract |

These later features are bounded by current authority rules; they are not silently enabled by being mentioned in the architecture.
