# MCP Contract

## Status

**Phase A semantic design. No MCP server is implemented; exact wire schemas and tool names remain Phase B decisions.**

This surface projects [Execution Model](EXECUTION_MODEL.md), [State Machines](STATE_MACHINES.md), [Authorization Model](AUTHORIZATION_MODEL.md), [Event Model](EVENT_MODEL.md) and [Failure Model](FAILURE_MODEL.md); it must not invent transport-specific lifecycle semantics.

MCP may be one control and observation surface for FLAMORIS AI Runtime, but the runtime kernel must remain transport-independent.

The Runtime is not only a remote workflow executor. It may own active model inference, jobs, workflow execution, interrupts, and real-time events.

## Design goals

An authorized caller should eventually be able to:

1. discover runtime/model/capability availability;
2. start bounded inference or a workflow-backed run;
3. observe execution in real time;
4. inspect active jobs;
5. request stop/cancel;
6. request pause/resume where supported;
7. interrupt a running inference at a defined control point;
8. retrieve results/provenance;
9. later, patch eligible unexecuted workflow state safely.

Transport authentication is not runtime authorization.

## Runtime control surface

Conceptual tools may include:

```text
capabilities.list
models.list

run.submit
run.status
run.result
run.cancel
run.interrupt

jobs.list
jobs.status
jobs.cancel

events.read / events.stream
```

Later:

```text
run.pause
run.resume
run.patch
```

Exact names are not frozen.

Avoid one giant `execute_anything` tool.

## Inference versus workflow submission

A caller may submit:

- a direct inference request;
- a workflow containing inference;
- a task that the Runtime/model expands into bounded registered work, depending on the future API.

Do not require the external caller to pre-author every internal job if the Runtime itself can safely coordinate jobs during inference.

Likewise, do not let model output become ambient permission to run arbitrary work.

Every dispatched capability remains subject to registration, authorization, resource, and side-effect policy.

## capabilities.list

Return machine-readable capabilities available to the caller.

Metadata may include:

- capability identifier/version;
- input/output schema;
- effect set;
- idempotency;
- cancellability;
- pause/resume support;
- resource class;
- availability;
- bounded resource hints.

Do not expose credentials, private endpoints, or private topology.

## models.list

Conceptually exposes model/backend choices relevant to the caller without leaking internal host details.

Possible metadata may include:

- logical model ID;
- context limits;
- supported modalities;
- streaming support;
- pause/resume support;
- rewind/state-mutation support where intentionally exposed;
- availability.

This tool is conceptual and may be merged into capability discovery later.

## run.submit

Admit work into the Runtime.

Submission must be revalidated at execution time. A compiled or cached Execution Plan is not an authorization grant and must not preserve stale permission.

At admission and every dispatch, retry and resume, check current capability pins/availability, caller authorization, concrete input scope, deadlines, budgets and policy. Effect-specific policy applies immediately before external/write/destructive/paid handoff. Validation success is not admission or proof of provider execution.

Potential inputs may include:

- request ID/idempotency key;
- model/task selection;
- prompt/input;
- optional Workflow IR;
- tighter client limits;
- trace/stream preferences.

The server assigns a `run_id`.

## run.status

Return bounded high-level execution state.

Run states and activity projection follow [State Machines](STATE_MACHINES.md): `created`, `queued`, `running`, `waiting`, `paused`, `cancelling`, `finalizing`, `succeeded`, `failed`, `cancelled`. The Run is an aggregate, never another scheduled Job.

Status includes bounded owner-visible metadata, sequence watermark, requested/applied controls, terminal result/error, external outcome certainty and cleanup/quarantine summary. A parent waiting for a running child does not make the Run paused. `finalizing` means useful work has ended while descendants/resource ownership is settled. Terminal is distinct from all physical resources having been confirmed free.

Unknown/expired instance IDs and incomplete retained histories are explicit; a new process must not pretend to have resumed a Run from a trace.

## run.interrupt

Request intervention in an active run/inference.

Conceptual actions may include:

- stop;
- pause;
- resume where supported;
- bounded external input injection;
- cancel selected child jobs;
- later, select/change an unexecuted workflow path.

The response should distinguish:

- interrupt accepted/requested;
- interrupt applied;
- unsupported action;
- run already terminal.

Interrupt application is observable as an event. Accepted/requested is not applied; duplicate commands are bounded and cannot repeat injection or dispatch. Job/Run version or equivalent commit-order arbitration prevents stale controls from reopening terminal work.

A graceful inference `stop` succeeds only if the backend/output contract supports bounded partial output. Otherwise reject it; `cancel` remains termination without a success result. Input injection must target an admitted bounded slot and pass current scope/schema checks.

## Jobs

Jobs are scheduler-visible runtime work.

A caller may need job visibility for live inspection and targeted cancellation.

Conceptual tools:

```text
jobs.list
jobs.status
jobs.cancel
```

Job details should expose bounded metadata such as:

- job ID/type;
- parent run/job;
- state;
- dependencies;
- resource class;
- timing/progress;
- terminal result summary/reference.

Do not expose secrets or unrestricted backend objects.

## Join and race visibility

Workflow execution may create join/race coordination.

Event/status output may expose:

- participants;
- waiting state;
- selected winner;
- loser cancellation requests;
- terminal loser outcomes where retained.

A race winner does not imply rollback of other side effects.

## Events and real-time observation

Structured events are the primary real-time observation surface.

Potential families include:

- inference lifecycle;
- prefill/decode progress;
- token generation where enabled;
- sampling metadata where enabled;
- model-exposed reasoning channel where policy permits;
- job lifecycle/progress;
- workflow node lifecycle;
- interrupt requested/applied;
- join/race state;
- resource warnings;
- terminal result.

Event streaming must be optional for correctness.

A reconnecting client inspects a current snapshot with sequence watermark and bounded retained history. Preserve atomic event-group boundaries; expired cursors and slow-subscriber overflow return explicit gaps. Optional telemetry loss cannot change execution. Replay remains an isolated read-only projection; no observation call can execute pending work.

## Trace levels

A request may ask for a supported trace level, bounded by deployment policy.

Conceptually:

1. lifecycle/state/timing;
2. token/sampling;
3. model-exposed reasoning stream;
4. deep backend debug probes.

Higher trace levels may be unavailable or restricted.

## Trace replay versus re-execution

If event-journal replay is exposed, **trace replay** must mean inspection/re-emission of retained structured events without invoking capabilities again.

It is distinct from retry or re-execution.

Conceptually, future surfaces may distinguish:

```text
trace.replay(run_id)
run.retry(run_id)
```

Exact names are not frozen.

Trace replay must not repeat paid calls, writes, destructive operations, external messages, or other side effects. Re-execution must pass normal authorization, effect, idempotency, and budget checks as a new execution attempt.

## Cancellation semantics

Cancellation must be explicit:

- cancellation may be best-effort for opaque remote capabilities;
- already completed side effects are not rolled back automatically;
- loser cancellation in `race` follows the same rule;
- the Runtime must not report a job/run as cancelled before its state reaches the defined terminal condition.

## Pause/resume semantics

Pause/resume is capability-specific.

A backend that cannot safely preserve model state must report pause/resume as unsupported.

The MCP surface must not pretend every remote provider has state-preserving pause. Run pause is a subtree barrier: report requested until all workload execution quiesces, reject unsupported in-flight operations, and bound the request wait. Targeted Job pause does not pause children. Resume clears only the applicable pause cause and does not reset deadlines or grant old permissions. Accounted cleanup may continue while workload is paused.

## Continuation visibility

Continuation is internal resume state owned by exactly one waiting/paused Job, not a second scheduler-visible object and not a requirement that every client manage resumable state manually.

Status/event surfaces may expose bounded continuation metadata when useful for inspection, such as:

- continuation ID;
- owning run/job;
- owning execution machine;
- waiting reason;
- resumable/unsupported state;
- deadline;
- resource/model affinity summary.

Do not expose raw backend pointers, KV cache contents, credentials, or unrestricted process state.

A normal caller should be able to submit a run and let the Runtime manage continuations automatically.

Job identity remains stable across yield/resume. Cancellation, timeout, provenance, metrics, and terminal state remain Job/run authority.

## Result handling

Final results contain validated declared outputs, stable metadata and known effect/cleanup evidence. A not-yet-terminal result request returns explicit pending state; race winners and token streams are provisional observations until the Run terminal result is committed. Future speculative replacement is not implied.

Timeout or response loss after an external handoff may return `outcome_unknown` or a timeout with unknown-outcome metadata. Do not expose a success/cancelled claim merely because the HTTP/MCP request ended.

Large media normally remains as references owned by the producing service.

Inference results may include final text/structured output plus explicitly enabled trace references.

## Authentication and authorization

Transport authentication and runtime authorization are separate.

Even after a caller is authenticated, the Runtime must evaluate:

- model access;
- capability access;
- side-effect permission;
- external provider permission;
- resource budgets;
- trace/logging permission.

Model output or workflow JSON never grants permission by itself.

## Retry and idempotency

Submission deduplication is scoped by authenticated subject/tenant, request kind, key and canonical input digest. Same key/digest returns the same Run within advertised retention; changed digest is a conflict. Expiry or non-durable restart does not prove non-execution and cannot promise exactly-once submission.

Provider operation idempotency is a separate adapter contract. A transport request ID alone is insufficient. Unknown handoff outcomes require bounded reconciliation or verified deduplication, never blind re-execution. Retry has fresh current checks, a bounded attempt identity and cumulative original Run budgets/deadline. Re-execution after terminal is a new Run with provenance. See [Authorization Model](AUTHORIZATION_MODEL.md) and [Failure Model](FAILURE_MODEL.md).

## Relationship to Agent

`flamoris-ai-agent` may call the Runtime.

The Agent remains authoritative for:

- identity;
- long-term memory;
- durable conversation;
- personality;
- goals/Agent policy.

The Runtime owns active model execution and runtime state, not durable Agent identity.

## Relationship to Intelligence MCP

`flamoris-intelligence-mcp` may expose or route intelligence capabilities, but AI Runtime may directly own model execution for the models/backends it controls.

The integration should preserve Runtime observability and interrupt semantics rather than forcing every model operation through an opaque remote request.

## Relationship to Generation and external capabilities

Generation MCP and other services retain their domain state.

External AI/API and MCP tools are exposed through configured registered capabilities.

Portable workflow/runtime requests must not carry arbitrary endpoints or raw credentials.

## Errors

Prefer stable error codes such as:

- `invalid_request`
- `unsupported_model`
- `backend_unavailable`
- `invalid_workflow`
- `capability_unavailable`
- `permission_denied`
- `budget_exceeded`
- `resource_unavailable`
- `duplicate_request`
- `run_not_found`
- `job_not_found`
- `unsupported_interrupt`
- `run_cancelled`
- `upstream_failure`
- `internal_error`

[Failure Model](FAILURE_MODEL.md) owns the full taxonomy/envelope, including `plan_stale`, `state_unavailable`, timeout, invalid/oversized result and `outcome_unknown`. Callers must not parse prose to identify error class. Raw provider bodies, credentials, private endpoints and host topology are never public errors. Current authorization applies separately to status, result, trace export and replay.

