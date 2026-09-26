# Phase B: C++ implementation design

**Proposal for review, 2026-09-27. Design documentation only; Phase C has not started.**
Targets [Issue #6](https://github.com/flamoris-jp/flamoris-ai-runtime/issues/6)
and the activation design input in
[Issue #5](https://github.com/flamoris-jp/flamoris-ai-runtime/issues/5).
The reviewed Phase A baseline is commit
[`776bf61`](https://github.com/flamoris-jp/flamoris-ai-runtime/commit/776bf61c937c27e78aad5e48d62f9b5c7dd2a8de),
merged through PR #4. Its detailed contracts remain semantic authority.

Phase B translates those contracts into proposed C++ responsibilities and testable
integration boundaries. It adds no production headers, build scaffold, backend,
tests, service, wire implementation or deployment configuration. The words
"must", "selected" and proposed type names describe future obligations.

## Reading map and authority

| Design question | Phase B document |
| --- | --- |
| Why this first backend? What do current runtimes actually expose? | [Backend research ADR](adr/0001-backend-control.md) |
| Which C++/build/test/platform baseline? | [Toolchain ADR](adr/0002-cpp-toolchain.md) |
| Who owns each object, reference and callback? | [C++ ownership](phase-b/CPP_OWNERSHIP.md) |
| Where do races linearize, and how is control capacity bounded? | [Concurrency](phase-b/CONCURRENCY.md) |
| How are inputs, plans, errors and events bounded and versioned? | [Errors and serialization](phase-b/ERROR_SERIALIZATION.md) |
| Which model state and control points must the adapter preserve? | [Backend contract](phase-b/BACKEND_CONTRACT.md) |
| When may resource capacity be reserved, reused or released? | [Resource/host contract](phase-b/RESOURCE_HOST_CONTRACT.md) |
| How can a non-durable Runtime enforce a hard paid budget? | [Durable paid-budget contract](phase-b/PAID_BUDGET_CONTRACT.md) |
| Who starts the process or loads a model? | [Activation contract](phase-b/ACTIVATION_CONTRACT.md) |
| How will every Phase A obligation be tested offline? | [A01–A42 test map](phase-b/ACCEPTANCE_TEST_MAP.md) |
| In what dependency order should code be written? | [Phase C slices](phase-b/PHASE_C_PLAN.md) |

Each document owns its implementation detail only. It cannot relax the Phase A
state machine, effect algebra, resource/paid accounting, current authorization,
bounded observation, or neighboring-service authority. Cross-document conflicts
must be fixed before implementing the affected slice. There is no stable public
or plugin ABI in this proposal.

## Selected direction

- C++20 with ordinary ownership and bounded typed results; a library-first,
  headless Kernel. The toolchain ADR specifies the proposed CI matrix and pins.
- One Runtime construction per process, enforced before workers/native calls;
  a new construction after failure/shutdown needs a fresh process. Its root owns
  global native init/log/free lifetime through final worker/callback quiescence.
  One control executor initially owns all per-Run commits, Scheduler,
  resource ledger and local policy projections. Workers own native state and I/O;
  only value observations return to the controller. This keeps dispatch, barrier,
  resource and event decisions locally atomic without a coroutine framework.
- Deterministic fake backend first, then an embedded llama.cpp **CPU** adapter
  under explicit conformance tests. This selects an integration direction, not
  validated backend support, GPU readiness or universal model compatibility.
- Private foundation research informs independent model/tokenizer/state and
  parity-test design. No private source/test migration or publication is approved
  by this proposal. No private code, paths or host topology are included.
- Always-on process activation is the initial deployment model, with cold loading
  owned by an ordinary admitted Job. On-demand host activation is a separately
  specified, conformance-gated option. Submit permission never implies service
  activation permission.
- An external durable paid authority reserves bounded liability and records
  possible handoff before provider execution. Restart cannot refund uncertainty,
  restore a Run, or turn a trace into an execution log.
- In-memory observation and inspection-only replay are baseline work. Durable
  journal recovery, state migration, universal rewind, speculative result
  replacement and distributed scheduling remain outside this implementation plan.

## Evidence and qualification boundaries

Primary source revisions and observation date are recorded in the backend ADR;
API existence is distinguished from adapter feasibility and measured guarantees.
Current private source was inspected through authenticated GitHub access;
public conclusions intentionally contain only generalized reuse decisions.
Historical workstation notes do not establish present host health, capacity,
backend conformance or service API behavior. This task makes no live-host claim.

| Remaining qualification | Gate before enabling the relevant capability | Does not block |
| --- | --- | --- |
| Native context/state/stop behavior on a pinned model and CPU build | Real-backend slice proves complete state tuple, allocation bounds, pause/cancel and cleanup; unsupported controls stay disabled | Offline domain, scheduler, compiler and fake-backend implementation |
| GPU or cross-process host arbitration | Verify enforceable host envelope/fencing and backend-specific memory/stop behavior; a free-memory sample or local lock is insufficient | CPU fake/reference semantics and non-device work |
| Hard paid-provider integration | Durable authority conformance, complete inventory and provider-enforced maximum liability; no concrete service/DB selected | Non-paid work and offline multi-instance fake budget tests |
| On-demand process activation | Host gateway start/dedup/drain/reconcile conformance, including admission race | Always-on Runtime design and model-loading tests |
| Private source reuse | Explicit source-publication rights and licensing/provenance review | Independently written interfaces and public-backend integration |
| Real toolchain/dependency builds | Pin dependencies, run clean configure/build/CTest and platform jobs in first Phase C slice | Review of the proposed build/test decisions |

These are explicit feature-enablement gates, not claims of successful integration
and not reasons to silently weaken Phase A. If evidence makes a required semantic
invariant infeasible, stop the affected work and submit a Phase A deviation with
replacement acceptance cases. The selected baseline currently requires no such
deviation; optional integrations remain disabled until their own evidence exists.

## Review and implementation handoff

Reviewers should challenge five boundaries together: lifecycle authority,
physical state ownership, current dispatch authorization, external uncertainty,
and observation retention. In particular, check the complete Job/Continuation
payload mapping, pre-admission versus lifecycle callback tickets, no-send proof
versus unknown handoff, all-or-none pause snapshot, and post-terminal ledger
updates after the stream closes.

The [test map](phase-b/ACCEPTANCE_TEST_MAP.md) defines every A01–A42 scenario and
its deterministic seams; the activation contract adds B-ACT01–B-ACT12 without
replacing those obligations. These are **test designs**, not executed tests.
Documentation/link checks cannot certify runtime semantics or native feasibility.

After review, Phase C follows the dependency-aware slice plan with small,
single-purpose commits. Implement only a slice's reviewed scope, run its stated
checks, and keep status limited to behavior backed by real code/tests. No
auto-merge or automatic transition to Phase C is part of this proposal.
