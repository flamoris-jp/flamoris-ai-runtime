# Backend and inference implementation contract

## Status and authority

Phase B proposal; no backend or production C++ declarations exist yet. This
document refines [Phase A state machines](../STATE_MACHINES.md) and
[resource semantics](../RESOURCE_MODEL.md), using [ADR 0001](../adr/0001-backend-control.md).
Names below are design responsibilities, not a stable ABI or wire schema.
The Job controller alone commits lifecycle changes. The Inference Machine
coordinates backend operations through the Runtime control executor; a worker
owns mutable native sessions and never changes a Job/Run directly.

## Minimum owned control surface

| Responsibility | Command input / returned observation | Required boundary |
| --- | --- | --- |
| Describe profile | Pinned backend revision, registered model identity, configuration and finite limits → capability descriptor | No allocation, model download, arbitrary path or dispatch |
| Estimate requirements | Profile, bounded context/output/step limits and current state summary → full resource vector and growth bound | Conservative bounds including model/context/logits/sampler/scratch; missing bound rejects admission |
| Activate model | Existing admitted Job's initializing segment, registered artifact reference and dispatch/resource permit → model handle, allocation receipts or bounded failure | No pre-Run warmup or independent loader; loading ownership follows activation contract |
| Create session | Model handle, tokenizer/template pins, bounded inputs, sampling/stop configuration and dispatch permit → unique session ID | At most one owning Job/attempt, one sequence initially; retain model reference |
| Tokenize | Bounded text/token input plus explicit special-token/template policy → bounded tokens and immutable metadata | Enforce context/token/byte limits before large allocation; no model-chosen config |
| Prefill segment | Session ID, segment identity, bounded token span and permit → progress/state/quiescence receipt or failure | Worker returns after complete chunk synchronization; no next chunk automatically |
| Decode segment | Session ID, next segment identity and permit → at most one accepted token candidate, state tuple, stop/progress and quiescence receipt | No autonomous generation loop, no token publication from native callbacks |
| Preserve in place | Last successful step receipt, suspension generation → frozen state-reference receipt | No copying required; no work in flight; retained allocations still charged |
| Resume | Exact state reference, compatibility pins and a fresh dispatch permit → next bounded segment | Same Job identity; no recreation from prompt on lost state |
| Request stop | Correlation and shared cancellation flag → request acknowledgement, later stopped/quiescent receipt | Request is not proof of stop; deadline does not free state |
| Inspect | Frozen state summary/statistics → bounded values | No tensor pointers or synchronous mutation of a running context |
| Release | State/model/allocation identity and cleanup permit → independent release receipts | Destruction waits for native readers/writers; unknown release goes to cleanup/quarantine |

Optional offload/snapshot/restore/rewind operations are separate capabilities,
not methods that always return apparent success. They have explicit source and
destination allocation identities, compatibility metadata, output byte bounds,
maintenance permits and completion evidence. The initial real profile does not
implement them. Opaque external providers use capability adapters and advertise
the absence of this controlled-step contract.

## Objects, ownership and lifetimes

| Design object | Owner / lifetime | Mutation and references | Disposal / serialization |
| --- | --- | --- | --- |
| BackendDescriptor / ModelProfile | Immutable registered snapshot pinned by plan; replacement gets a new revision | Control executor reads values, workers receive immutable copies/references | Safe bounded metadata only; no native addresses or local artifact paths |
| NativeBackendLifetime | Sole Runtime composition root under ProcessRuntimeGuard; before first native call through complete teardown | One designated worker performs process-global init/log configuration/free; stable bounded log context; Resource Manager owns a separate process-overhead ledger record | Internal; dispose after native handles/callbacks quiesce and settle global footprint once; no per-Job/global-free coupling |
| BackendWorker | Runtime backend host, through shutdown/cleanup | One bounded work queue; owns session table, uses the root-owned native lifetime | Join only after drained; never detach a thread holding freed native state |
| ModelHandle | Worker model table with session and bounded residency references; initial load belongs to one admitted Job | Immutable logical model; all native access serialized initially | Free only after all sessions/calls stop; Resource Manager confirms actual release |
| InferenceSession | Worker table, uniquely associated with Job/attempt; may survive in cleanup | Native context, sampler and incremental decoder; one in-flight segment; logical access by generation-tagged ID | No public serialization. Transfer destruction duty to cleanup if Job closes |
| InferenceStateSummary | Control-owned bounded copy of last accepted receipt | Positions/stage/stop state and allocation IDs, no mutable native aliases | Authorized observations may serialize; not a checkpoint |
| StateReference | Resource Manager reference identities plus owning Job resume payload | Continuation → PendingResume → active ownership transfer; native session stays on worker | Nonportable, process-scoped; no lease; invalidation removes resumability, not memory |
| SegmentCommand / SegmentReceipt | Owned queue value, retained through controller decision | Runtime incarnation, Job/attempt/segment and backend/host generations | Internal only; bounded result/error data copied or moved, not borrowed |
| StopToken / CallbackContext | Shared worker-operation control block | Atomic stop request only; callback cannot invoke controller or own a Run pointer | Native operation retains it until no callback can run; never serialize |

Model weights may be shared only where the profile proves it safe. The initial
worker serializes access even across separate contexts, avoiding an unproved
upstream reentrancy assumption. One worker does not imply one Job: frozen
sessions may coexist, with each retained footprint charged. Later parallel
workers or shared device pools require another qualified profile.

## Process-global native lifecycle

Phase C baseline permits **one RuntimeInstance construction per process**,
including fake-only production configurations. The composition-root factory
atomically claims a `ProcessRuntimeGuard` before creating executors/workers or
calling any native API. Concurrent or subsequent construction returns a bounded
availability error before native calls. The claim remains consumed through
shutdown or failed construction; restart/recreation requires a new process.
This intentionally excludes embedded multi-Runtime hosts and same-process native
reinitialization. There is one process guard shared by all entry points; loading
independent copies of the library or mixing independently managed llama.cpp users
in that process is unsupported. Process-local exclusivity grants no host GPU rights.

The root owns exactly one `NativeBackendLifetime` for the initial llama.cpp
integration, with immutable configuration, initialization status and a bounded
sanitized logging context. A designated backend worker performs the global calls
under this owner. Initialize at most once before its first native model operation,
triggered by an admitted Job's initializing segment. Before dispatch, reserve the
complete incremental vector, including a separately bounded process-owned global
initialization allowance and the Job's cold model/context/loading allowance. The
Resource Manager creates a `RuntimeOverheadRecord` keyed by this process/backend
incarnation before native init; the initiating Job is only the trigger and never
owns the persistent global allocation. On native materialization, atomically
convert the global reservation into process-owned allocation/uncertainty records;
release only proven unmaterialized remainder. Count their unique physical bytes
and handles once until matching global free/release evidence at process shutdown.
The per-Run quota still covers its entire working set (model, context and state),
while global overhead is charged to a separately configured, bounded Runtime/host
shared-overhead allowance, not the initiating tenant's Run quota. If the policy
cannot fund that allowance or the backend cannot bound the global footprint,
reject the first dispatch before native init. Job cancellation, failure, terminal
and model unload do not release the global record; later Jobs reuse it subject to
physical-capacity checks, without reserving it a second time. A cancellation
racing init still leaves the process-owned obligation until confirmed release or
containment. Repeated model loads/unloads do not initialize/free global backend
state. The pinned
[llama.h source](../adr/0001-backend-control.md) documents `llama_backend_init()` /
`llama_backend_free()` as program-level calls and global `llama_log_set()` as
not thread-safe. This lifetime scope is an adapter obligation, not an upstream
promise that arbitrary initialization failures are recoverable.

Shutdown/failed-start cleanup follows this strict order:

1. Close useful admission/dispatch and request stop; keep the native owner and
   log/callback contexts alive. No other Runtime can start during drain.
2. Prove native calls/callbacks stopped, then destroy sessions, contexts, samplers
   and models on their designated worker, with ordinary allocation receipts.
3. On that worker, call `llama_backend_free()` exactly once only if initialization
   completed and native teardown is safe. Keep logging context alive through free.
   Settle the process-owned global allocation once only after matching native
   release evidence, not when the initiating Job or last model terminates.
4. After all native calls/log producers are quiescent, detach the global log
   callback, then release its context and native owner, and finish worker shutdown.
   No log reconfiguration races an active native call. The process guard stays spent.

A partial initialization with uncertain global state or a wedged native call
retains the process-owned reservation/materialization uncertainty and requires
containment/process exit; do not blindly call free, declare bytes available,
discard callback storage or release the process claim. Failed second construction cannot free or
reconfigure the first instance's backend. Destructors neither retry initialization
nor infer quiescence from an empty queue. Log callbacks carry only their stable
bounded context, never a Run/Job/Runtime pointer.

## Complete inference state tuple

The preserved tuple contains model/tokenizer/template and backend/configuration
pins, context/sequence identity, committed input history within bounds, evaluated
token position, sampled/emitted output position, optional sampled-but-not-yet-
evaluated token, sampler state including RNG/penalty/grammar history, stop-matcher
state, incremental text-decoder bytes, machine stage and retained allocations.

The two positions are intentionally different. llama.cpp's normal pattern
evaluates input, samples and accepts a token, then evaluates that token on the
next iteration. Therefore an emitted token can be pending as the next input.
Pause/resume preserves that pending token and the already-advanced sampler
together; it must neither sample twice nor ingest the token twice. Output UTF-8
may span tokens; decoder carry bytes and bounded stop-sequence lookbehind belong
to the tuple too. A cache dump alone is never advertised as the full state.

The Runtime commits observation and the tuple's logical version together, then
publishes authorized token events. Native state may have advanced before receipt
acceptance, but the worker is frozen and cannot take another useful step. If
cancel/deadline wins before the receipt commits, suppress its token publication,
mark the native state cleanup-only and reconcile its allocations. Do not pretend
the native call rolled back. Retries do not reuse this unaccepted state.

## Worker protocol and safe points

1. The control executor validates current policy, deadline, capability pins,
   state compatibility, host/backend epochs and complete resource admission.
   It creates one permit and segment identity, with reserved receipt/event
   capacity, before sending work. A permit is scoped to this segment only.
2. The worker validates the permit identity against its session, polls the
   stop token, and performs at most the bounded segment. Initial prefill uses
   configured finite chunks; decode emits at most one token candidate.
3. On success it synchronizes native execution and captures the complete tuple
   and allocation/progress facts. It posts a reserved bounded receipt and waits
   for a new command. Native logits are consumed or copied within this operation;
   borrowed backend buffers never escape into a controller/event queue.
4. The controller checks freshness and the current monotonic deadline at receipt
   processing, then atomically commits progress or suspension and its event
   group. Safe-point receipt precedes Continuation creation; the continuation
   and waiting/paused transition precede publishing released execution capacity.
5. Resume follows normal authorization/resource admission and sends a fresh
   segment only after consuming the Continuation into PendingResume and reaching
   running. A frozen session itself cannot wake or enqueue its Job.

Pause stops at a **successful** synchronized segment boundary. A stop request
received halfway through an iteration remains pending until that boundary for
preserved pause. CPU abort may accelerate cancellation, but is not used to
manufacture a preserved pause: the inspected `llama_decode` contract allows
completed ubatches to remain after abort or fatal error. Such a session becomes
cleanup-only unless a future profile proves and tests exact recovery.

Quiescence has stage-specific meaning: tokenization/load completion, synchronized
prefill chunk, or decode/sampling/state update completion. A loading operation
that cannot preserve partial progress advertises that limitation to pause
preflight; it does not claim a reusable half-loaded state. Bound operation sizes
and measure latency in qualification; no universal sub-token or GPU-stop time is
promised. If native work wedges, retain the session and accounting, close affected
dispatch, and use the configured supervisor/host containment path. An in-process
writer cannot safely be fenced just by dropping its callback.

## Capabilities and first real profile

Descriptors carry support **per backend/model/configuration revision**, finite
input/context/output/step bounds, safe points, stop behavior, native state format
revision, threading mode, accounting strategy and optional controls. They are
not arbitrary flags supplied by workflow/model output.

| Capability | First llama.cpp CPU qualification target | Denied / future |
| --- | --- | --- |
| Input / model | Approved decoder-only text model and matching vocabulary; bounded registered preprocessing | Unqualified model families, arbitrary endpoints, multimodal/encoder paths |
| Prefill / decode | Fixed maximum prefill chunk; single-sequence decode | Speculative/multi-token autonomous advancement |
| Sampling | Greedy baseline; explicitly seeded supported native chain with retained state | Arbitrary user code/plugins, unbounded logits tracing |
| Cancel | Poll before/after segment; CPU abort optional for stopping with state invalidation | Claiming GPU sub-token abort from the CPU callback |
| Pause / resume | In-place preservation at advertised successful boundaries, same session/pins | Restart recovery, evacuation, reconstruction after cache loss |
| Child yield / result injection | Bounded approved text/token insertion at an advertised inference boundary; controller validates schema, context capacity and policy before next admitted prefill segment | Model output invoking tools directly or unbounded/opaque tensor injection |
| Snapshot / offload / rewind | Advertised unavailable initially | Assuming sequence export preserves RNG or frees buffers; universal partial-cache removal |
| Batching / resources | One context/sequence per Job; conservative context/model pools, bounded worker queue | Internal continuous batching bypassing controller selection |
| Observations | Runtime monotonic timing, positions, bounded selected sampling/token details | Reliance on example-only perf helpers, raw tensors, private native logs |

The initial injection-qualified profile uses a registered bounded text-to-token
append policy, basic greedy/seeded sampling and optional repetition penalties;
grammar-constrained sampling is excluded from that profile. First ingest any
already committed pending token (already accepted by the sampler), then append
validated injected tokens in order. Accept each injected token into the penalty
history exactly once, without drawing RNG. Preserve the output decoder and
output-only stop matcher; injected input is not emitted output. Only sample again
after the final injected prefill chunk. Never re-accept the old pending token,
rewrite committed text, or silently apply an unpinned chat template. Qualification
tests pin this treatment with and without a pending token. A model/profile that
does not support this append policy advertises injection unavailable, and a plan
requiring it is rejected before dispatch. Future grammar/tool-message policies
need their own explicit state treatment and tests.

## Memory and resource receipts

Requirements include shared model allocation, private context/state, configured
logits/output buffers, sampler/decoder/history, native graph scratch, queue/event
storage and uncertainty allowance. Reserve the complete bounded vector before
materialization. Where native suballocation is opaque, charge a conservative
pool once and enforce profile sublimits; do not sum its members again physically.
Run working-set quotas still include required shared state.

Native allocation failures remain possible after admission and map to resource
failure with actual partial materialization reconciled. A profile lacking a
defensible upper bound is unavailable under that envelope; free-memory polling
cannot substitute for it. A later allocator/telemetry integration may tighten
the bound without changing the ledger contract.

A synchronized paused context has relinquished execution capacity, not state
memory. Native context deletion, model unload and optional transfers each return
separate generation-tagged allocation receipts. A release acknowledgement can
settle its matching cleanup record after Job terminalization; it never revives
the Job or releases a newer allocation reusing a raw address. Source state stays
charged during copy/snapshot and until confirmed release. Retained state blocking
a required child results in supported bounded offload or `resource_deadlock`;
the baseline must fail explicitly when offload is unavailable.

## Callback, error and teardown rules

Backend callbacks can only read their stable cancellation control block or emit
bounded owned observations. They cannot throw through a C boundary, acquire a
Run lock, allocate an unbounded result, publish tokens directly, or dispatch a
child. The root-owned NativeBackendLifetime registers global logging once before
native initialization and detaches only after teardown quiescence as specified
above. Sanitize and cap copied diagnostics before observation.

All receipts carry runtime incarnation, backend/model generation, Job/attempt,
segment and relevant host/allocation epochs. Stale lifecycle proposals are
discarded. Independently valid resource-release evidence still reaches the
matching Resource Manager cleanup record. Duplicate delivery cannot repeat
sampling, injection, continuation consumption or capacity release.

Native status/exception boundaries map to the stable Phase A errors: invalid
input/profile → input/availability; capacity/allocation → `resource_unavailable`;
invalid preserved state → `state_unavailable`; native execution →
`backend_failure`; malformed/oversized output → `invalid_result`/`result_too_large`;
unconfirmed destruction → cleanup error with quarantined debt. Cancellation and
deadline classification remains controller-owned. Unknown external provider
outcomes remain an adapter concern, never inferred from local backend stopping.

Shutdown closes new commands, requests stop, drains reserved receipts and waits
for native calls before session/context/sampler/model destruction. Sampler-chain
ownership is honored: a chain owns inserted samplers; do not free them twice.
Stop tokens and callback contexts outlive the last callback. A destructor cannot
block the control executor indefinitely or detach a still-writing worker; the
configured supervisor is the fallback when safe containment fails.

## Deterministic tests and real qualification

The script fake exposes explicit tokenize/prefill/decode/safe-point/stop/release
steps, caller-driven completion order, failure injection and counted allocation
receipts. No threads, weights, network or GPU are needed to prove lifecycle
semantics. Cover one in-flight segment, duplicate/stale receipt rejection,
cancel-before-receipt and deadline-at-receipt, no step while frozen, pending-token
exactly-once resume/injection, RNG/decoder carry preservation, retained memory
after yield, abort partial state, late release, and lost acknowledgement.

The first real CPU profile additionally compares uninterrupted and paused runs
under fixed fixture/configuration/seed; checks stage boundaries, full tuple and
token order; verifies context limits and conservative memory envelope; tests
cancel/abort/error cleanup; and measures safe-point latency. Fixtures are opt-in,
checksummed and separately licensed. CPU/GPU floating-point outputs across
different configurations are not promised bit-identical. GPU/profile-specific
qualification is a separate gate; A01–A42 architectural acceptance does not
depend on a working GPU or paid provider.
