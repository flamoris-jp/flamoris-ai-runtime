# Phase B: dependency-aware Phase C delivery plan

## Status and entry gate

**Implementation plan only. No slice, build, test or native compute described here is
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

The baseline has distinct evidence gates: deterministic kernel tests, native
CPU correctness/model control qualification, and native OpenCL compute parity.
External host activation needs its own evidence. Strict cross-crash paid-cost
control is an optional future integration and does not gate this baseline.
A fixture unavailable for a real lane is recorded as not run, never passed.

## Dependency and responsibility index

`→` below is a prerequisite relationship. C01 is the root. Every later slice
inherits its prerequisites' completed regression suite.

| Slice | Direct prerequisites | Main responsibility | Acceptance families exercised |
| --- | --- | --- | --- |
| C01 | Reviewed Phase B | Toolchain, IDs, bounded values/errors/effects, test kit | A06; B-SER01 primitives |
| C02 | C01 | Run/Job controller, Continuation, event commit and manual executor | A08–A09, A14–A19, A28, A37–A38, A40 controller cases; B-LIFE01/B-EVENT01/B-NATIVE01 instance/holder cases |
| C03 | C02 | Resource ledger, host protocol, accounted cleanup | A10–A13, A28–A29; B-CALL01/B-DRAIN01 resource cases |
| C04 | C03 | Single-user authorization, submission claim and finite Run limits | A04–A05, A24–A27, A33, A35, B-RETRY01; A41–A42/B-PAID02 optional |
| C05 | C01, C04 | Bounded IR parser, validator/compiler, direct normalization | A01–A03, A06–A07, A24, A36, A39 compile/admission cases; B-SER01 |
| C06 | C03, C04, C05 | Scheduler, fairness, Runtime activation and host lifecycle client | A10, A13, A15, A17–A19, A34, A37, A40; B-ACT01–B-ACT12 |
| C07 | C02, C04, C05, C06 | WorkflowMachine, dependency binding, await/join/race | A07–A10, A16, A20–A26, A36–A40 workflow cases |
| C08 | C03, C04, C05, C06, C07 | InferenceMachine and scripted controllable native worker | A03–A04, A07–A10, A14–A20, A28, A38–A40; B-INPUT01 |
| C09 | C02, C03, C04, C07 | Retained observation, gaps, bounded reconciliation, inspection replay | A29–A32, A35; B-EVENT01/B-SER01 observation cases |
| C10 | C03, C06, C08 | FLAMORIS native CPU reference and conformance qualification | Native-model conformance suite; related A08–A19/A28 invariants |
| C10a | C10 | FLAMORIS native OpenCL compute and CPU/OpenCL parity | Native parity/qualification suite |
| C11 | C04, C05, C07, C08, C09 | Registered adapters and transport-facing projection | A25–A27, A31–A33, A35, A39, external outcome boundary regression |
| C12 | C01–C11 plus C10a | Complete baseline integration and evidence report | All baseline A cases (A41–A42 optional), supplemental cases and qualified native CPU/OpenCL suite |

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
- **Excluded:** Scheduler, actual native worker, native inference bindings, service host,
  stable ABI, speculative allocators/coroutines and copying private implementation.

## C02 — lifecycle and atomic local commits

- **Scope:** composition-root Runtime-owned control executor,
  Run/Job records/controllers,
  AttemptRecord, Continuation/PendingResume move ownership, pause causes/barriers,
  immutable finalizing intent, callback identities and bounded command/proposal
  slots. State plus complete EventGroup commit is one bounded control turn.
- **Prerequisites:** C01. Use manually acknowledged test ports for resources and
  native-worker evidence until C03/C08 replace those seams; never infer physical release.
- **Tests / acceptance:** complete transition table and invalid pairs; owner and
  generation rejection; both cancel/completion orders; exact deadline boundary;
  Run versus targeted pause projection and atomic child/barrier races. Exercise
  A08–A09, A14–A19, A28, A37–A38, A40 plus B-LIFE01/B-EVENT01 at controller level
  and B-NATIVE01 native holder lifetime with fake operations; C10 closes real qualification.
- **Commit units:** records/ownership; serialized proposal and event publication;
  suspension/control/finalization; deterministic arbitration/failure tests.
- **Excluded:** useful real work, native pause claims, resource feasibility,
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
  and release loss; source/target overlap; stale host/native-worker epochs; fake host
  grants versus unenforced capacity hints; unsafe native containment. A10–A13,
  A28–A29 with actual ResourceManager, B-CALL01/B-DRAIN01 lifetime checks, and
  cross-Run full-vector contention and shared model residency accounting without partial useful execution.
- **Commit units:** accounting records/admission; allocation/reference lifetime;
  host epochs/receipt protocol; quarantine/reconciliation and fault tests.
- **Excluded:** device reset/service control implementation, pretending existing
  GPU Node Manager supports a new API, real offload/allocator, GPU utilization
  heuristics, residency optimization and automatic recovery after crash.

## C04 — single-user admission, policy and finite Run limits

- **Scope:** current per-capability/effect authorization, confirmation binding,
  local cumulative child/attempt/resource limits, keyed submission dedup and
  independent unkeyed admission. No tenant database or durable money ledger.
- **Prerequisites:** C03.
- **Tests:** A04–A05 (local Run limit), A24 (finite group allowance), A25–A27,
  A33/A35 (current owner/scope), B-RETRY01. Test revocation versus dispatch,
  duplicate claims, unknown external outcomes and retry refusal.
- **Commit units:** authorization values; submission claims; limits and effect
  dispatch; deterministic failure/race tests.
- **Excluded:** tenant isolation, cross-crash monetary guarantees and Paid
  Budget Authority. Its former A41–A42 and B-PAID02 belong to a separate task.

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
  an unavailable host authority cannot be replaced by local optimistic state.
- **Tests / acceptance:** A10, A13, A15, A17–A19, A34, A37, A40; B-ACT01–B-ACT12:
  concurrent startup, initiating waiter cancellation, partial acquisition failure,
  model-cold admission, host changes, idle/new-submit ordering, loader cancellation,
  stale completion, authorization separation, restart inventory, ambiguous forwarded
  submission and non-pausable/stuck native loading.
- **Commit units:** queue/fairness and eligibility; resource/dependency deadlock;
  process activation client contract/fake gateway; model-loading ownership/dedup;
  idle/drain races.
- **Excluded:** Continuation scheduling, startup as an inferred submit permission,
  system-specific service manager, distributed scheduling, GPU compute, performance
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

## C08 — InferenceMachine with scripted native worker

- **Scope:** tokenize/prefill/bounded decode/sample/state-commit progression under
  an existing Job, explicit native execution-profile capabilities, current-state checks,
  safe-point stop/pause/resume, bounded dynamic proposal/result injection and
  callback fencing. Scripted native fake supplies deterministic state/sampler/callback
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
- **Commit units:** native execution contract/capability values; scripted native worker harness;
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

## C10 — FLAMORIS native CPU reference and causal-text execution

- **Scope:** independently implement registered model, `ProcessorDefinition` and
  `TokenizerDefinition` identities and loading, bounded tokenize/prefill/decode,
  explicit KV/cache and sampler/algorithm state,
  deterministic CPU compute, segment safe points and release receipts. Preserve
  model-neutral state envelope separately from causal-text profile state.
- **Prerequisites:** C03/C06/C08 and approved fixture identity/license/checksum.
- **Tests:** B-REAL01–B-REAL04/B-REAL06–B-REAL07 as revised below; known tokenizer
  vectors, strict UTF-8/Unicode and emoji round trips, changed-tokenizer plan and
  resume rejection, cached versus uncached CPU correctness, uninterrupted versus
  paused progression, partial failure, cancellation, shared model lifetime and
  bounded allocations.
- **Commit units:** model/processor/tokenizer/artifact identity; CPU ops; incremental state
  and sampler; Inference Machine integration; lifetime/conformance fixtures.
- **Excluded:** copying private source, embedded third-party runtime, OpenCL
  claims, portable checkpoints or universal model-family support.

## C10a — native OpenCL compute and parity

- **Scope:** implement FLAMORIS-owned OpenCL operations under the same native
  model execution and resource accounting. Device-specific state compatibility
  must be explicit; CPU remains the correctness oracle.
- **Prerequisites:** qualified C10 and actual OpenCL build/device evidence.
- **Tests:** CPU/OpenCL parity under declared numeric tolerances using identical
  pinned tokenizer/processor identities and input token IDs; cached/uncached
  decode, repeated load/release, cancellation and resource/quiescence receipts.
  A historical hardware note cannot establish this gate.
- **Commit units:** device/context ownership; bounded operations; parity fixtures;
  native conformance and honest capability metadata.

## C11 — registered capability and caller adapters

- **Scope:** bounded transport-independent control/observation facade, registered
  capability adapter implementations needed for the selected baseline, and thin
  MCP/API/CLI projections. Pin each chosen transport/dependency after inspecting its
  current primary contract. Preserve same submission, result, pause and outcome
  semantics; no handler owns a separate Job or dispatch authority.
- **Prerequisites:** C04/C05/C07/C08/C09. C10 is required only for real inference
  demonstrations, not for facade contract tests with a scripted native worker.
- **Tests / acceptance:** A25–A27, A31–A33, A35, A39, external outcome handling through in-process
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
  qualify pinned native CPU and OpenCL profiles; update README/capability/status only from code
  and tests that exist. Publish supported versus unsupported controls and required host external contracts.
- **Prerequisites:** C01–C11 plus C10a. If a component is deliberately not delivered, narrow
  the milestone label; do not call the complete baseline accepted.
- **Tests / acceptance:** every baseline A case in [test map](ACCEPTANCE_TEST_MAP.md),
  B-LIFE/B-CALL/B-EVENT/B-RETRY/B-INPUT/B-SER/B-DRAIN, B-NATIVE01 (B-PAID02 optional), B-ACT01–B-ACT12, clean
  configure/build and required compiler/static/sanitizer gates. Run native CPU qualification and OpenCL parity on approved fixtures/devices;
  report an absent fixture/device as not run, not pass and withhold that claim. Inspect final diff and public/private boundaries.
- **Commit units:** cross-layer regressions per defect; lifecycle/resource fixes;
  adapter conformance fixes; documentation/evidence update. Do not squash an entire
  implementation into one unreviewable working-tree change.
- **Excluded:** performance tuning without correctness evidence, unqualified GPU claims, external paid-provider exercises without a
  dedicated task, durable Run recovery and deployment/automatic merge.

## Native qualification matrix

These are proposed tests, not passing results. Record fixture license/checksum,
model/configuration, CPU/OpenCL implementation, device/driver and supported
profile for each actual run.

| ID | Required evidence | Gate |
| --- | --- | --- |
| B-REAL01 | Licensed tiny model and pinned processor/tokenizer load; known token vectors, bounded tokenize, prefill, decode and sample; CPU reference outputs checked. | Native CPU fixture |
| B-REAL02 | Same profile/input/seed: uninterrupted and safe-point paused/resumed state/output progression agrees. | Native CPU fixture |
| B-REAL03 | Stop at segment boundaries really quiesces before state release; unsupported partial-step control rejects. | Native CPU fixture and failure hooks |
| B-REAL04 | Multiple independently owned Runtime instances may construct/destroy without cross-release; shared native model/context allocations stay charged until the last reference and actual release. | CPU fixture and allocator hooks |
| B-REAL05 | Offload/snapshot/rewind/batching remain false until separately qualified. | Capability rejection suite |
| B-REAL06 | Causal-text pending token, sampler/RNG, penalty/grammar, decoder carry and bounded injection remain exactly once across pause. | Native CPU causal-text fixture |
| B-REAL07 | Strict UTF-8 and registered tokenizer encode/decode vectors cover `日本語`, `😀`, `👩‍💻`, `❤️`, `👍🏽`, `🇯🇵`, decomposed `が` and precomposed `が`, token-split UTF-8 carry and pause/resume. Reject malformed/final incomplete sequences, unpaired surrogate conversion, special-token collisions and changed processor/tokenizer plan or resume identity. Round trips follow pinned normalization; no fixed token count per grapheme. | Native CPU tokenizer/decoder fixture |
| B-OPENCL01 | Native OpenCL operations match CPU oracle within declared tolerance, with bounded allocations and valid cancellation/release evidence. | Actual qualified OpenCL device |
| B-HOST01 | External host authority enforces capacity and generation fencing across processes. | Separate host integration |
| B-PAID01 | Optional future strict-cost authority and provider enforce durable maximum liability. | Separate optional integration |

## Completion recording and stop conditions

Each slice PR records its reviewed source revision, scope, implemented acceptance
IDs, exact commands actually available at that revision, results and unmet
integration gates. Commands are added when the real build layout exists; this
design does not advertise hypothetical setup commands as current instructions.

Keep acceptance IDs visible in test names/metadata, with a bounded fixture per
listed failure/interleaving, so coverage can be audited without counting Markdown
headings as tests. Record blocked cases explicitly. A test-framework green status
with an empty/skipped native-model lane cannot support a native execution capability claim.

Stop the affected part and seek design review when implementation would require
an additional scheduler identity, revived terminal state, unaccounted resource or
paid liability, changed external effect semantics, missing host authority, or
relaxed authorization. Continue independent authorized slices where their
contracts remain valid; do not conceal a semantic deviation in an adapter.
