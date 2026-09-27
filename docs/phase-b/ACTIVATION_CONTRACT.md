# Activation and Startup Contract

**Phase B design for Issue #5; no process launcher, model loader or service manager is
implemented.** This translates existing [authority](../ARCHITECTURE.md),
[resource](../RESOURCE_MODEL.md), [lifecycle](../STATE_MACHINES.md) and
[failure](../FAILURE_MODEL.md) boundaries. It adds no Job transition and no independent
scheduler-visible activation work. Names below are internal design values, not new MCP
tool names or an assertion about a deployed GPU Node Manager API.

## Four different boundaries

| Boundary | Authority and selected baseline | Request permission / result |
| --- | --- | --- |
| Process activation | Host/service authority starts the configured Runtime process; always-on is the first deployment mode | Separate activation permission; process readiness says nothing about admitted Runs/models |
| Backend/model loading | Existing admitted Job's `initializing` segment under Resource Manager admission and backend worker ownership | Current model/capability/resource scope; success makes a compatible resident model available |
| Run admission | Kernel admission authority compiles/validates and atomically claims submission identity | Current submit permission, finite limits and registry/resource availability checks; one Run |
| Job dispatch | Kernel control executor/Scheduler plus Resource Manager | Current per-attempt eligibility, full resource vector and generation checks; one execution lease |

Always-on keeps the service available; it does not eagerly load model weights. On-demand
process activation is a supported *integration design option* outside the Kernel. It is
enabled only when a configured host gateway satisfies the contract below. Until then a
cold endpoint returns bounded unavailable status; Runtime does not shell out, start
arbitrary services or treat `run.submit` permission as host activation permission.

Agent, Studio, ChatGPT and other callers use the same configured gateway/transport
authorization. Identity or model text never provides implicit activation authority.
No portable workflow can name a host service, executable, arbitrary model path, endpoint
or credential. Logical model IDs resolve through trusted pinned configuration.

## Process state belongs to the host gateway

`ActivationKey` binds logical service/deployment configuration revision and expected
host ownership scope. `ActivationGeneration` and a host epoch distinguish attempts.
The host gateway owns bounded activation records, request waiters and service lifecycle;
the Kernel does not own or reconstruct them. Readiness includes the new Runtime instance
identity and an admission epoch so a stale successful launch cannot route new submissions
to the wrong process.

| Host-observed state | Transition / condition | Contract |
| --- | --- | --- |
| Stopped | Authorized trigger atomically claims key/generation | Starting; exactly one host start operation for compatible concurrent triggers |
| Starting | Fresh matching process handshake and required initialization complete | Ready; publish instance/admission epoch to authorized waiters |
| Starting | Request waiter cancels/disconnects | Detach that waiter only; do not kill shared activation or fabricate a Run cancellation |
| Starting | Startup deadline, explicit host abort or failure | Stopping/failed cleanup; release confirmed resources, retain unknown obligations; no readiness claim |
| Ready | Host requests bounded drain/idle stop | Admission gate closure negotiated with Kernel; transition to draining only when acknowledged |
| Draining | Existing work settles or drain deadline initiates cancellation | Stop only after confirmed safe shutdown or explicit supervisor containment |
| Stopping | Matching stop/reconciliation acknowledgement | Stopped; a late callback cannot publish Ready for the old generation |
| Any | Ownership/epoch lost or process status uncertain | Unknown/unavailable until reconciliation; no duplicate replacement launch |

Failure status preserves the bounded cause and unresolved ownership. A later authorized
retry uses a new activation generation only after prior process/partial startup work is
stopped or safely contained. Gateway shutdown/restart must rediscover actual process
ownership before it creates a replacement; absence of its own record is not proof of stop.
The protocol does not require Runtime to implement host-side durable recovery.

The start operation has a configured finite deadline independent of an individual
waiter's transport timeout. Its finite waiter cap/bytes/retention prevents request floods
from creating unbounded queues. Same key/configuration shares one attempt; conflicting
configuration does not mutate an in-flight start and returns a bounded conflict.
Authorization is checked before attaching a waiter and again before the host action.
Observing an existing activation does not grant permission to stop it.

Cancelling the last waiter does not itself stop the process. The host's preconfigured
activation policy may request bounded abort/idle stop, which is separately serialized
against startup completion. If startup succeeds, the process may remain idle even though
no waiter remains. Explicit service stop requires host authorization and normal drain.

## Readiness, pre-Run resources and submission

Process bootstrap may consume bounded host-owned RAM/process handles/control capacity
before a Run exists. The host activation record owns and reconciles those costs; a
Runtime that partially starts reports them to the host contract before claiming ready.
The baseline performs no model load, inference, provider handoff or paid work
during pre-Run bootstrap. Failure cannot leak these obligations into a nonexistent Run.

Kernel readiness means its bounded queues/control executor/configuration are initialized,
its process incarnation is known, and it can make honest admission decisions. GPU envelopes have their own readiness flags: an unavailable device blocks
only work needing it; CPU/non-device work may proceed under its own authority. Resource startup
reconciliation remains mandatory before affected capacity is usable.

An on-demand gateway holds only a bounded *request waiter* until process readiness, not
a Run, Job, Execution Plan grant or scheduler entry. A cancelled waiter receives no Run
unless admission already committed. After Ready, it forwards an explicitly bounded
submission to the indicated instance/epoch and the ordinary compiler/admission path.
Activation success does not imply submission success or extend caller deadlines.

Concurrent requests share process activation but preserve their optional submission
keys/digests. Explicit valid keys deduplicate at Kernel admission, independently of
activation; absent keys create independent fresh admissions. If forwarding has an
unknown outcome, use an already known Run identity or the explicit key against the
same live instance within retention. Without either, report unknown admission rather
than replaying the unkeyed submit. Never assume failure and silently submit to a new
process. After
restart the gateway reports the ambiguity because baseline submission deduplication is
not durable. A confirmed rejection before Run creation permits a bounded retry; it is
not an unknown handoff.

## Cold model loading remains Job work

The existing Job becomes `running` only after normal dispatch checks and complete resource
admission. Its machine may then report `initializing` while its worker loads weights and
creates bounded backend state. No hidden loader thread acquires capacity outside the
Job's lease. The complete incremental vector includes cold weights, transient loading
buffers and context/state capacity. If this is the first native use, admission also
reserves a separate bounded process-owned global initialization allowance; its
physical footprint transfers to the `RuntimeOverheadRecord` on materialization,
not to the initiating Job/Run quota. Partial or uncertain global materialization
remains charged until proven release or process containment. Model/context
materialization remains Job/residency-owned as below. Workload deadlines continue
through load.

A Resource Manager `ModelResidencyRecord` maps a content revision, backend/configuration
fingerprint, logical device and backend/host incarnation to:

| Residency state | Owner / meaning |
| --- | --- |
| Absent | No usable allocation; not a free-capacity promise |
| Loading | Exactly one existing Job/attempt owns load, its lease and finite acquisition deadline |
| Resident | Valid compatible shared allocation charged once; Run working-set quotas still apply |
| Releasing | No new references admitted; actual bytes charged until acknowledgement |
| Unknown | Failed/partial/stale allocation is unavailable and accounted until reconciliation |

This record is resource bookkeeping, never another Job. Scheduler chooses the loader
among eligible Jobs using ordinary fairness. Other Jobs for the same model remain queued
without execution leases or Continuations; residency change only triggers eligibility
reevaluation, not dispatch. They retain their own deadlines and cancellation scope. A
failed load is not an automatic retry on behalf of every waiting Job.

For initial simplicity, loading ownership never transfers mid-attempt. If the loader
Job is cancelled before completion commits, stop/reconcile that load through its normal
cleanup. Other queued Jobs may compete for a fresh load only after the old attempt is
stopped/contained and resource accounting permits it. A load result accepted first may
publish a resident model; later cancellation disposes the cancelled Job's state/reference
while cache retention follows the explicitly bounded residency policy. It cannot erase
the Job's accepted cancellation or perform new inference for that Job.

The shared cache does not outlive physical ownership evidence. Host/backend epoch loss
invalidates current compatibility, fences new consumers and initiates reconciliation.
No loader can pause unless the selected backend advertises a safe preservation point;
Run pause containing a non-pausable load is rejected per A17. No invented Continuation
stands in for an incomplete backend allocation.

Eager/pre-Run model warmup, detached background prefetch, loading shared across processes,
and transferring a running loader between Jobs are excluded. A future explicit warmup
must be an authorized admitted Job under a reviewed capability/plan, not a Kernel
startup shortcut. These optional extensions are not required for Phase C baseline.

## Idle shutdown and admission race

Always-on is the baseline default; automatic idle exit is disabled. For a configured
on-demand deployment, the host gateway owns the finite idle timer and decision to stop.
Kernel supplies a bounded idleness/admission snapshot and negotiates closure; it never
decides to stop unrelated services.

Idleness requires no nonterminal Jobs, pending admission owners/duplicate waiters,
prepared dispatches, model loads, controller maintenance, or outstanding cleanup that
cannot be transferred under the host contract. Retained terminal observations do not
count as active work, but their advertised in-memory availability ends on process stop.
Warm cache still owns resources: closing admission is followed by acknowledged cache
eviction/cleanup, not assumed zero allocation.

On the single control executor, `prepare_idle_stop(expected_admission_epoch)` atomically
checks these facts and closes new admission. A submission that claims admission first
prevents idle closure; closure first rejects subsequent admission as unavailable with
no Run. Reopening a draining instance implicitly is excluded. The gateway waits for
stop/reconciliation, starts a new incarnation if authorized, and may retry only requests
proven not admitted. Missing closure acknowledgement is unknown and cannot authorize
a replacement process. Monotonic deadlines bound drain and separate cleanup allowances;
uncontainable native work is escalated to the supervisor without fictitious terminal
or resource-release evidence.

## Status, access and failure

Host process activation exposes bounded authorized status under activation identity,
host revision and generation, outside a Run event stream. It never invents Run/Job
events before admission. Model loading after admission uses ordinary Job-stage/resource
observations in that Run's ordered stream. Shared residency status exposes logical
availability, never the user's prompt, private path or raw handle.

Process unavailable/start failure maps to `backend_unavailable` or configured transport
availability response with bounded startup reason. Model load failure maps to
`backend_failure`/`resource_unavailable`; an actual Job deadline uses `job_timeout`.
Unsupported pause remains a rejected control request. Status must separate process
ready, model resident, resource admission possible, paid scope ready and actual Job
dispatch; none implies all the others. Access to activation controls, Run submit,
models, status, trace and host diagnostics is checked separately and revalidated at use.

## Supplemental deterministic acceptance

These Phase B test obligations complement A01–A42 without changing their semantics.
Use a fake monotonic clock, manually stepped host gateway, process handshake, resource
ledger/backend and policy; no system service, GPU, model weight, network or billing is
required. A fake process boundary changes instance identity and loses Runtime state.

| ID | Competing actions / injected failure | Required observation and accounting |
| --- | --- | --- |
| B-ACT01 | Two authorized cold triggers for the same key; conflicting revision also arrives | One start generation/host call; compatible waiters share it; conflict creates no second process or Run |
| B-ACT02 | One/all waiters cancel before/after startup success | Detach waiters only; no fabricated Run cancel; process readiness/host idle policy remains authoritative |
| B-ACT03 | Startup partially acquires resources then fails/times out | Host record retains/releases confirmed obligations; unknown residue blocks replacement; no Ready/Run events |
| B-ACT04 | Process is ready, model cold, two eligible Jobs need it | One Job owns bounded loading; other remains queued without lease; one model allocation and both Run quotas |
| B-ACT05 | Host epoch changes during startup or model loading | Fence old readiness/dispatch; preserve old uncertainty; new incarnation/envelope only after reconciliation |
| B-ACT06 | Idle gate closure races with new submission claim | Exactly one wins control order; admitted claim prevents closure, closed gate creates no Run; no blind resubmit |
| B-ACT07 | Loading Job cancel competes with load result, other Job waits | No ownership transfer or duplicate load; cleanup first if cancel won; resident result retains accounting if accepted first |
| B-ACT08 | Old/duplicate startup/stop callback arrives after new generation | No stale readiness, stop of replacement, second callback effect or accidental resource release |
| B-ACT09 | Submit-authorized caller lacks activation/model/inspect permission | Deny each missing scope at its own boundary; no host call, leaked status or inferred permission |
| B-ACT10 | Process ready after restart, resource envelope incomplete | Affected device admission blocked; independent permitted work available; no old Run recovery |
| B-ACT11 | Submitted request response is lost, then process restarts | Explicit unknown admission/outcome; activation dedup does not claim durable submission exactly-once |
| B-ACT12 | Run pause during non-pausable load; shutdown with stuck native loader | Pause rejected without partial barrier; no fake quiescence/deallocation/terminal, supervisor owns containment |

No Phase A semantic deviation is needed for the selected baseline. If implementation
requires pre-Run inference, scheduler-visible activation records, automatic work replay,
implicit service-start authority or loader ownership transfer, stop that portion and
propose a separate reviewed Phase A deviation with affected A cases. Real on-demand
integration stays disabled until host start/stop/arbitration conformance is verified.
