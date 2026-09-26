# Phase B: dependency-aware Phase C delivery plan

## Status and entry gate

**Implementation plan only. No slice, build, test or backend described here is
implemented by Phase B.** Start Phase C after review of Issue #6 and this design.
The reviewed Phase A contracts remain semantic authority. A discovered semantic
defect requires a documented, reviewed deviation and affected acceptance updates
before implementing a different behavior.

Use the current reviewed main at each slice, read `AGENTS.md`, and inspect current
dependencies rather than assuming an earlier PR has merged. Each slice should
produce a focused PR with tests, documentation of actually implemented behavior,
and meaningful single-purpose commits. Do not auto-merge. The ordering below
allows preparation in parallel, but a dependent slice must not invent an interface
or silently substitute a pending design.

The baseline has two distinct evidence gates:

1. **Kernel semantics:** all A01–A42 plus supplemental implementation/activation
   cases pass offline with controllable fakes. In-memory inspection replay is
   included. A fake does not prove a real integration's capabilities.
2. **First-backend qualification:** embedded llama.cpp CPU integration passes the
   separately bounded real-model suite at a pinned revision with approved model
   fixtures. Report exactly which controls are supported. GPU and deployed
   host/provider integrations each need their own additional evidence.

A kernel-only milestone may be published honestly as such. It is not the completed
real-inference baseline. Lack of an external durable budget implementation keeps
hard-budget paid capabilities disabled; lack of enforceable host arbitration keeps
affected device execution disabled. The Runtime must not invent either authority.

## Dependency and responsibility index

`→` below is a prerequisite relationship. C01 is the root. Every later slice
inherits its prerequisites' completed regression suite.

| Slice | Direct prerequisites | Main responsibility | Acceptance families exercised |
| --- | --- | --- | --- |
| C01 | Reviewed Phase B | Toolchain, IDs, bounded values/errors/effects, test kit | A06; B-SER01 primitives |
| C02 | C01 | Process construction guard, Run/Job controller, Continuation, event commit and manual executor | A08–A09, A14–A19, A28, A37–A38, A40 controller cases; B-LIFE01/B-EVENT01/B-NATIVE01 factory cases |
| C03 | C02 | Resource ledger, host protocol, accounted cleanup | A10–A13, A28–A29; B-CALL01/B-DRAIN01 resource cases |
| C04 | C03 | Current authorization, submission claim, Run limits, durable paid port | A04–A05, A24–A27, A33, A35, A41–A42 policy/port cases; B-RETRY01/B-PAID02 |
| C05 | C01, C04 | Bounded IR parser, validator/compiler, direct normalization | A01–A03, A06–A07, A24, A36, A39 compile/admission cases; B-SER01 |
| C06 | C03, C04, C05 | Scheduler, fairness, backend activation and host lifecycle client | A10, A13, A15, A17–A19, A34, A37, A40; B-ACT01–B-ACT12 |
| C07 | C02, C04, C05, C06 | WorkflowMachine, dependency binding, await/join/race | A07–A10, A16, A20–A26, A36–A40 workflow cases |
| C08 | C03, C04, C05, C06, C07 | InferenceMachine and scripted controllable backend | A03–A04, A07–A10, A14–A20, A28, A38–A40; B-INPUT01 |
| C09 | C02, C03, C04, C07 | Retained observation, gaps, bounded reconciliation, inspection replay | A29–A32, A35; B-EVENT01/B-SER01 observation cases |
| C10 | C03, C06, C08 | Embedded llama.cpp CPU backend and conformance qualification | Real-backend suite; related A08–A19/A28 invariants |
| C11 | C04, C05, C07, C08, C09 | Registered adapters and transport-facing projection | A25–A27, A31–A33, A35, A39, A41–A42 boundary regression |
| C12 | C01–C11 | Complete baseline integration and evidence report | All A01–A42, supplemental cases and qualified real-backend suite |

Some scenarios appear in several rows deliberately: a controller unit test cannot
establish end-to-end compiler/dispatch behavior. The closing suite must call the
production component at the named boundary, with fakes only at external seams.
Substituting an already-decided fake lifecycle outcome for the controller under
test does not close an acceptance case.

## C01 — build, bounded domain primitives and deterministic test kit

- **Scope:** implement the reviewed toolchain/layout ADR, core library/test targets,
  compiler matrix and offline dependency acquisition; strong identities/epochs,
  finite limits, checked counters, `EffectSet`, `ErrorEnvelope`, clock interface,
  immutable event values and test-only deterministic ID/clock sources.
- **Prerequisites:** Phase B review, including researched licensing/reuse decisions.
- **Tests / acceptance:** exhaustive valid/invalid effect sets and aggregation
  (A06), strong-ID/version mismatch, integer-bound overflow, typed error sanitizing,
  fake clock with independent timer delivery. Configure/build from clean state and
  run the selected framework on every required toolchain; no model/network needed.
- **Commit units:** build/CI foundation; bounded domain/error/effect types; test
  fixture support and meaningful contract tests. Keep test-only sources outside
  production libraries and do not add an unnecessary binary host yet.
- **Excluded:** Scheduler, actual backend, native inference bindings, service host,
  stable ABI, speculative allocators/coroutines and copying private implementation.

## C02 — lifecycle and atomic local commits

- **Scope:** composition-root ProcessRuntimeGuard and Runtime-owned control executor,
  Run/Job records/controllers,
  AttemptRecord, Continuation/PendingResume move ownership, pause causes/barriers,
  immutable finalizing intent, callback identities and bounded command/proposal
  slots. State plus complete EventGroup commit is one bounded control turn.
- **Prerequisites:** C01. Use manually acknowledged test ports for resources and
  backend evidence until C03/C08 replace those seams; never infer physical release.
- **Tests / acceptance:** complete transition table and invalid pairs; owner and
  generation rejection; both cancel/completion orders; exact deadline boundary;
  Run versus targeted pause projection and atomic child/barrier races. Exercise
  A08–A09, A14–A19, A28, A37–A38, A40 plus B-LIFE01/B-EVENT01 at controller level
  and B-NATIVE01 construction rejection with fake native calls; C10 closes native teardown qualification.
- **Commit units:** records/ownership; serialized proposal and event publication;
  suspension/control/finalization; deterministic arbitration/failure tests.
- **Excluded:** useful real work, backend pause claims, resource feasibility,
  remote service calls, transport server and durable history.

## C03 — resources, host evidence and cleanup ownership

- **Scope:** one instance ledger for requirements, full-vector reservation,
  materialization, unique allocation/reference accounting, execution leases,
  transfer overlap, cleanup/quarantine and resource revisions. Implement the
  HostAuthorityPort contract with fail-closed unavailable adapter and deterministic
  fake; preserve no-host-reset boundary. Cleanup owns evidence after Job lifetime.
- **Prerequisites:** C02; bounded control/closure capacity must exist before a
  quarantine transfer can permit terminal publication.
- **Tests / acceptance:** physical versus per-Run shared quotas; partial acquisition
  and release loss; source/target overlap; stale host/backend epochs; fake host
  grants versus unenforced capacity hints; unsafe native containment. A10–A13,
  A28–A29 with actual ResourceManager, B-CALL01/B-DRAIN01 lifetime checks, and
  cross-Run full-vector contention without partial useful execution.
- **Commit units:** accounting records/admission; allocation/reference lifetime;
  host epochs/receipt protocol; quarantine/reconciliation and fault tests.
- **Excluded:** device reset/service control implementation, pretending existing
  GPU Node Manager supports a new API, real offload/allocator, GPU utilization
  heuristics, residency optimization and automatic recovery after crash.

## C04 — admission, current policy and durable paid protocol

- **Scope:** revocable current policy/capability projection, Run-local cumulative
  budget and confirmation checks; atomic scoped submission claim plus one reserved
  Run ID; independent fresh unkeyed PendingSubmission records; bounded keyed
  same-digest waiters; admitted-claim retention; external
  PaidBudgetPort adapter contract, proof-of-no-handoff release, settlement,
  full paid-race `reserve_envelope`/`bind_attempt` funding gates with nested
  subset ownership, durable `arm_handoff`/ticket protocol, bounded DeliveryGate
  registry/cleanup ownership and atomic close/send with identity-bound no-send
  proof, inventory/reconciliation and fail-closed
  behavior. Async preparation receipts
  never become independently spendable grants; final local dispatch checks commit
  together under the control executor.
- **Prerequisites:** C03. D fake storage is outside Runtime ownership so a new
  Runtime construction can test durable liabilities without recovering Runs.
- **Tests / acceptance:** revoke at each boundary; simultaneous same/conflicting
  digest for explicit keys, independent sequential/concurrent unkeyed submissions
  and invalid present keys; pre-Run rejection waiter release ordering; two Runs competing for one
  tenant budget; lost reservation receipt; restart/unavailable/partial inventory;
  aggregate paid-race funding before any participant handoff, including failure
  of the last reservation; current handle/trace access. Close policy/port cases
  in A04–A05, A24–A27, A33,
  A35, A41–A42, B-PAID02 and B-RETRY01; retain integrated retests in C07/C08/C11.
- **Commit units:** policy/scope and budget values; submission claim/waiters;
  durable paid protocol/fake; eligibility/handoff guards; retry/uncertainty tests.
- **Excluded:** concrete durable budget database/service, automatic refunds,
  unbounded monetary estimates, provider implementation, cross-restart submission
  exactly-once and Run recovery.

## C05 — parser, validator, compiler and normalized submission

- **Scope:** implement the reviewed bounded IR representation/reference grammar,
  duplicate-key/version rejection, normalized immutable plan/canonical fingerprint,
  capability pins, schema/data dependencies, effect ordering and conservative
  dynamic child/race envelopes. Direct inference normalizes through the same
  compiler contract. Compiler performs no ambient input resolution or dispatch.
- **Prerequisites:** C01 and C04; registry/policy values already have explicit
  owners and lifetime. Build against the current reviewed serialization contract.
- **Tests / acceptance:** A01–A03, A06–A07, A24, A36, A39 compile/admission cases,
  B-SER01 bounded malformed input tests, canonical fixtures and repeatable rejection.
  Include semantically meaningful changes, non-semantic JSON ordering, reference-
  derived cycles and unordered conflicting read/write operations.
- **Commit units:** bounded parser/IR; schema/reference/effect validator;
  normalization and pins/fingerprint; direct inference normalization; fixtures.
- **Excluded:** executing JSON directly, hidden filesystem/network reference reads,
  live Continuation serialization, arbitrary expressions, graph patching/loops,
  automatic plan migration and transport wire stability claims beyond the reviewed
  internal compatibility contract.

## C06 — deterministic Scheduler and activation

- **Scope:** ready Jobs only, priority/FIFO/aging policy, complete-vector dispatch,
  retained-state dependency deadlock detection, bounded maintenance/reclaim, and
  the activation design's separate process/model/Run/Job boundaries. Implement
  model-load records, client-side process readiness/generation fencing and idle
  drain/admission arbitration under the configured host contract. Always-on is the
  baseline; process start/dedup records belong to the external host gateway and are
  exercised through a deterministic gateway fake, not a new Kernel service manager.
- **Prerequisites:** C03/C04/C05. Actual service startup remains an external port;
  an unavailable host/paid authority cannot be replaced by local optimistic state.
- **Tests / acceptance:** A10, A13, A15, A17–A19, A34, A37, A40; B-ACT01–B-ACT12:
  concurrent startup, initiating waiter cancellation, partial acquisition failure,
  model-cold admission, host changes, idle/new-submit ordering, loader cancellation,
  stale completion, authorization separation, restart inventory, ambiguous forwarded
  submission and non-pausable/stuck native loading.
- **Commit units:** queue/fairness and eligibility; resource/dependency deadlock;
  process activation client contract/fake gateway; model-loading ownership/dedup;
  idle/drain races.
- **Excluded:** Continuation scheduling, startup as an inferred submit permission,
  system-specific service manager, distributed scheduling, GPU backend, performance
  residency reordering and useful work during a pause barrier.

## C07 — WorkflowMachine and bounded coordination

- **Scope:** execute compiled dependency/binding nodes, immutable invocation inputs,
  child ownership and counters, bounded pure control steps, await/all-success/
  all-settled, fixed-participant race and bounded child fragment registration.
  Machines propose changes; JobController commits lifecycle and authority.
- **Prerequisites:** C02/C04/C05/C06; every dispatch uses existing guards and ledger.
- **Tests / acceptance:** A07–A10, A16, A20–A26, A36–A40 through production workflow
  execution with fake adapters. Run Phase A's complete MCP-child failure trace;
  enumerate simultaneous race candidates and unacceptable results, all-settled
  declared-order output, uncertain losers and subtree-before-parent terminal order.
- **Commit units:** node readiness/bindings; child/await/join; race winner/loser
  ownership; dynamic fragment validation; integrated failure/fault tests.
- **Excluded:** arbitrary scripts, detached children, automatic compensating writes,
  mutable live plans, provisional winner replacement, speculative execution and
  unrestricted recursive tool calls.

## C08 — InferenceMachine with scripted backend

- **Scope:** tokenize/prefill/bounded decode/sample/state-commit progression under
  an existing Job, explicit backend capability descriptors, current-state checks,
  safe-point stop/pause/resume, bounded dynamic proposal/result injection and
  callback fencing. FakeBackend supplies deterministic state/sampler/callback
  evidence; it does not emulate a real model's quality or universal capabilities.
- **Prerequisites:** C03/C04/C05/C06/C07. Resource and child bookkeeping cannot be
  postponed behind inference execution.
- **Tests / acceptance:** A03–A04, A07–A10, A14–A20, A28, A38–A40 end-to-end through
  compiled plans; B-INPUT01/B-CALL01. Assert no useful step while paused, matching
  complete state tuple across an uninterrupted versus paused scripted execution:
  evaluated/emitted positions, pending token, sampler/RNG/penalty/grammar history,
  decoder carry and stop matcher. Verify pending-token/injected-input exactly-once
  behavior, cancellation before native receipt publication, partial-step abort
  invalidation and safe cleanup.
- **Commit units:** backend contract/capability values; scripted backend harness;
  inference state progression; safe-point controls; child injection and lifecycle
  conformance. Keep tests independent of model weights/network/GPU.
- **Excluded:** llama.cpp implementation, claims of generic rewind/offload/snapshot,
  universal mid-kernel preemption, remote `generate()` pretending to own decode,
  cross-process state persistence and token-string golden tests as architecture proof.

## C09 — baseline observation and inspection replay

- **Scope:** bounded retained complete groups, current snapshot watermarks,
  authorized event read/stream/export, subscriber gaps, optional telemetry loss,
  bounded same-Run post-terminal reconciliation/closure, and isolated in-memory
  replay with no live execution ports.
- **Prerequisites:** C02/C03/C04/C07. Mandatory event publication already exists in
  C02; this slice adds observation/retention policy, not a late replacement for the
  lifecycle event authority. C08 traces join the regression suite when available.
- **Tests / acceptance:** A29–A32, A35 with production EventStore/ReplayProjection;
  fill every storage class, withhold subscribers, expire cursors, inject incomplete
  inspection groups and assert exactly zero replay dispatch/cleanup/paid mutations.
  B-EVENT01/B-SER01 enforce publication and redaction bounds.
- **Commit units:** retention and snapshots; bounded subscriber/gap projection;
  post-terminal closure; isolated replay; access/redaction/overflow tests.
- **Excluded:** durable journal persistence, crash recovery, event-sourced execution,
  replay as retry and unbounded debug/tensor logging. A32 must pass before baseline
  acceptance regardless of any later durable-journal plan.

## C10 — first real backend, embedded llama.cpp on CPU

- **Scope:** pin researched llama.cpp revision and licenses, implement the minimum
  embedded C API adapter, ProcessRuntimeGuard enforcing one construction per
  process, root-owned NativeBackendLifetime for global init/log/free ordering,
  one sequence/context per Job and serialized context
  ownership, explicit tokenizer/model identity, bounded prefill/decode/sampler
  progression, in-place preserved-state safe points and accounted release.
  Revalidate primary API evidence if the pinned revision changes.
- **Prerequisites:** C03/C06/C08 and review of real fixture license/checksum/resource
  bounds. No production feature is advertised merely because the fake supports it.
- **Tests / acceptance:** run the real-backend qualification matrix below and
  relevant lifecycle invariants from A08–A19/A28; CPU model tests are a separate
  opt-in, clearly reported integration lane. B-NATIVE01 uses instrumented native
  calls and isolated process/factory tests before real integration. Default contract tests remain offline
  with no weights. Unsupported offload/rewind/batching must reject explicitly.
- **Commit units:** pinned dependency/licensing; model/context owner and resource
  adapter; bounded step/sampler controls; pause/cancel/lifetime conformance;
  qualified fixture and accurately scoped status documentation.
- **Excluded:** GPU acceleration, continuous batching, cross-Job shared KV, durable
  snapshots, offload/rewind, copying private model code and unsupported speed claims.

## C11 — registered capability and caller adapters

- **Scope:** bounded transport-independent control/observation facade, registered
  capability adapter implementations needed for the selected baseline, and thin
  MCP/API/CLI projections. Pin each chosen transport/dependency after inspecting its
  current primary contract. Preserve same submission, result, pause and outcome
  semantics; no handler owns a separate Job or dispatch authority.
- **Prerequisites:** C04/C05/C07/C08/C09. C10 is required only for real inference
  demonstrations, not for facade contract tests with a scripted backend.
- **Tests / acceptance:** A25–A27, A31–A33, A35, A39, A41–A42 through in-process
  fake transport and faulting registered adapter; unauthenticated/unauthorized
  callers, lost response after accepted operation, oversized streamed output,
  disconnect without implicit cancellation, current observation access and expiry.
- **Commit units:** caller facade/versioned mapping; one registered adapter at a
  time; chosen thin transport; output streaming/gaps and boundary fault tests.
- **Excluded:** broad third-party connector catalog, arbitrary URLs/credentials in
  workflows, Generation domain ownership, durable Agent state, live paid calls in
  CI and platform deployment scripts unsupported by current source inspection.

## C12 — integrated baseline and honest release evidence

- **Scope:** run complete compiled-plan-to-control scenarios with real kernel
  components and external fakes; repair integration defects in focused commits;
  qualify the pinned CPU backend; update README/capability/status only from code
  and tests that exist. Publish supported versus unsupported controls and required
  host/budget external contracts.
- **Prerequisites:** C01–C11. If a component is deliberately not delivered, narrow
  the milestone label; do not call the complete baseline accepted.
- **Tests / acceptance:** every A01–A42 case in [test map](ACCEPTANCE_TEST_MAP.md),
  B-LIFE/B-CALL/B-EVENT/B-RETRY/B-INPUT/B-SER/B-DRAIN, B-PAID02/B-NATIVE01, B-ACT01–B-ACT12, clean
  configure/build and required compiler/static/sanitizer gates. Run optional real
  CPU qualification with explicitly available fixture; report an absent fixture as
  not run, not pass. Inspect final diff and public/private boundaries.
- **Commit units:** cross-layer regressions per defect; lifecycle/resource fixes;
  adapter conformance fixes; documentation/evidence update. Do not squash an entire
  implementation into one unreviewable working-tree change.
- **Excluded:** performance tuning without correctness evidence, mandatory GPU
  tests for architecture acceptance, external paid-provider exercises without a
  dedicated task, durable Run recovery and deployment/automatic merge.

## Real integration qualification matrix

These supplement, rather than replace, the A cases. All fixture identity, model
revision/checksum, backend commit, configuration and capability claims are recorded
with results. Real output quality is not an architecture assertion.

| ID | Required evidence | Environment / gate |
| --- | --- | --- |
| B-REAL01 | Approved tiny model/tokenizer loads within conservative measured envelope; tokenize → bounded prefill → decode → sampler → committed token progression is observable. | CPU, provisioned licensed weights; no download inside deterministic suite. |
| B-REAL02 | Same pinned build/config/seed/input: uninterrupted versus in-place paused/resumed execution retains matching token/state progression at advertised quiescent boundaries. | CPU fixture; no cross-device numerical identity promise. |
| B-REAL03 | Cancel at tokenize/prefill/decode boundaries produces genuine quiescence before context destruction/release; no next useful step after applied stop. Unsupported partial-step control rejects honestly. | CPU fixture with safe-point instrumentation and sanitizer lane where supported. |
| B-REAL04 | Sole-instance process guard and root-owned global init/log/free ordering (including callback context alive through free); context/model/sampler lifetime, partial load failure, retained state, unload acknowledgement and finite memory growth are accounted; state invalidation never becomes silent fresh inference. | CPU fixture and controlled allocator/failure seams; native hang containment remains A28/supervisor boundary. |
| B-REAL05 | Snapshot/offload/rewind/continuous batching capabilities remain false until independently implemented and qualified; opaque providers do not inherit embedded controls. | Capability metadata/conformance; no weights needed for rejection checks. |
| B-REAL06 | With and without a pending emitted token, approved bounded child-input append consumes that token and injected penalty history once, preserves sampler/RNG/decoder/stop state, and samples only after the last admitted prefill chunk; rejected grammar/injection profiles cannot silently substitute another strategy. | CPU fixture under a pinned injection-qualified profile and current policy/resource checks. |
| B-HOST01 | Deployed authority actually enforces ownership/capacity and generation fencing, including another process attempting allocation; unavailable/expired authority blocks affected dispatch. | Separate host integration task; no success claim from process-local mutex. |
| B-GPU01 | Chosen device/backend build, resource envelope, safe-point/quiescence latency and release/epoch handling satisfy the same controls under actual acceleration. | Separate GPU qualification after B-HOST01; no hardware assumption from historical notes. |
| B-PAID01 | Chosen external authority demonstrates durable atomic reservation, crash inventory, idempotent settlement and failure behavior; provider enforces the declared finite liability. | Separate durable-authority/provider contract qualification; fake D alone cannot enable hard paid budgets. |

## Completion recording and stop conditions

Each slice PR records its reviewed source revision, scope, implemented acceptance
IDs, exact commands actually available at that revision, results and unmet
integration gates. Commands are added when the real build layout exists; this
design does not advertise hypothetical setup commands as current instructions.

Keep acceptance IDs visible in test names/metadata, with a bounded fixture per
listed failure/interleaving, so coverage can be audited without counting Markdown
headings as tests. Record blocked cases explicitly. A test-framework green status
with an empty/skipped real-backend lane cannot support a backend capability claim.

Stop the affected part and seek design review when implementation would require
an additional scheduler identity, revived terminal state, unaccounted resource or
paid liability, changed external effect semantics, missing host authority, or
relaxed authorization. Continue independent authorized slices where their
contracts remain valid; do not conceal a semantic deviation in an adapter.
