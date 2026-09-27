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
| Why native execution? What can comparative runtime research teach us? | [Backend research ADR](adr/0001-backend-control.md) |
| Which C++/build/test/platform baseline? | [Toolchain ADR](adr/0002-cpp-toolchain.md) |
| Who owns each object, reference and callback? | [C++ ownership](phase-b/CPP_OWNERSHIP.md) |
| Where do races linearize, and how is control capacity bounded? | [Concurrency](phase-b/CONCURRENCY.md) |
| How are inputs, plans, errors and events bounded and versioned? | [Errors and serialization](phase-b/ERROR_SERIALIZATION.md) |
| Which native execution state and control points do we own? | [Backend contract](phase-b/BACKEND_CONTRACT.md) |
| When may resource capacity be reserved, reused or released? | [Resource/host contract](phase-b/RESOURCE_HOST_CONTRACT.md) |
| How might an optional strict-cost integration work? | [Durable paid-budget contract](phase-b/PAID_BUDGET_CONTRACT.md) |
| Who starts the process or loads a model? | [Activation contract](phase-b/ACTIVATION_CONTRACT.md) |
| How will every Phase A obligation be tested offline? | [A01–A42 test map](phase-b/ACCEPTANCE_TEST_MAP.md) |
| In what dependency order should code be written? | [Phase C slices](phase-b/PHASE_C_PLAN.md) |

Each document owns its implementation detail only. It cannot relax the Phase A
state machine, effect algebra, resource accounting and optional paid integration, current authorization,
bounded observation, or neighboring-service authority. Cross-document conflicts
must be fixed before implementing the affected slice. There is no stable public
or plugin ABI in this proposal.

## Selected direction

- C++20, headless single-user Kernel. FLAMORIS owns native model execution,
  model-neutral state, resource lifetime and workflow control. CPU reference is
  the correctness oracle; OpenCL is the first native accelerated compute path.
- One control executor initially commits Run/Job, scheduler, ledger and event
  transitions. Workers own native state and I/O. This is an implementation
  choice, not a permanent one-Runtime-construction-per-process rule.
- Deterministic fake first, then native CPU incremental execution, then OpenCL
  and CPU/OpenCL parity. Causal text is the initial execution profile, not the
  universal state definition. Existing private `flamoris-LLM` informs concepts
  and test methodology; source migration needs publication/provenance clearance.
- Third-party runtimes/providers are comparative research or registered Workflow
  external capabilities. There is no interchangeable inference backend adapter.
- Always-on process activation initially; cold model loading belongs to an
  admitted Job. On-demand host activation is a separately qualified option.
- Finite Run limits and current capability authorization remain mandatory.
  Durable Paid Budget Authority is an optional future strict-cost profile and
  not a baseline completion gate; tenant accounting/isolation is out of scope.
- In-memory observation/replay is inspection only. Durable Run recovery,
  universal rewind and distributed scheduling remain outside this phase.

## Evidence and qualification boundaries

Primary source revisions and observation date are recorded in the backend ADR;
API existence is distinguished from adapter feasibility and measured guarantees.
Current private source was inspected through authenticated GitHub access;
public conclusions intentionally contain only generalized reuse decisions.
Historical workstation notes do not establish present host health, capacity,
backend conformance or service API behavior. This task makes no live-host claim.

| Remaining qualification | Gate before enabling the relevant capability | Does not block |
| --- | --- | --- |
| FLAMORIS native CPU state/stop behavior on a pinned model | Native CPU slice proves profile-specific complete state, allocation bounds, pause/cancel and cleanup; unsupported controls stay disabled | Offline domain, scheduler, compiler and fake-backend implementation |
| GPU or cross-process host arbitration | Verify enforceable host envelope/fencing and backend-specific memory/stop behavior; a free-memory sample or local lock is insufficient | CPU fake/reference semantics and non-device work |
| Optional strict-cost paid-provider integration | Durable authority conformance, complete inventory and provider-enforced maximum liability; no concrete service/DB selected | Baseline native and ordinary registered capability work |
| On-demand process activation | Host gateway start/dedup/drain/reconcile conformance, including admission race | Always-on Runtime design and model-loading tests |
| Private source reuse | Explicit source-publication rights and licensing/provenance review | Independently written interfaces and native implementation |
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
