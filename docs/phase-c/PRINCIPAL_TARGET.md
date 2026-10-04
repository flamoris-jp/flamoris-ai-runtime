# Isolated principal Runtime target

`PrincipalRuntimeTarget` is a trusted non-MCP embedding boundary for an internal
application or an optional Agent.
Its factory takes an authenticated subject and exclusive ownership of one
`RuntimeInstance`. Subject and instance ownership remain fixed for the target's
lifetime. The embedding must assign distinct process-incarnation identities to
independent instances; there is no global instance allocator in this wrapper.

Every command rejects a different or revoked subject before calling the instance:
submit, pinned submit, status, result, cancel, pause, resume, events and replay.
The original authorization context passes unchanged to the kernel, which still
checks expiry, current policy, capability/effect authority, access surfaces and
Run ownership. The wrapper neither authenticates portable subject strings nor
widens grants. An upstream transport must authenticate the caller and select the
configured target without trusting a caller-selected owner identifier.

Ordinary transport adapters receive `RuntimeCommandPort`. The extra
`trusted_runtime()` seam is an administrative capability for compiler preparation,
host observations and instance management. It must remain inside the trusted
embedding; neither this seam nor pinned admission is added to public control JSON.
The wrapper does not add jobs, receipts, retries or an independent deduplication
store. All such state stays inside its owned instance.

Offline acceptance uses real Runtime instances to show that every foreign-subject
command returns `permission_denied`, current expiry/revocation still rejects, and
two different subjects can use the same request/idempotency key without sharing a
Run. Each target retains its own duplicate receipt. No provider invocation occurs
without host receipts in these tests.

This is source-level target isolation, not deployed two-user qualification. Authenticated application-to-target mapping remains an embedding obligation.
The retired Generation composition bridge does not authorize a new adapter or
root-delegation mechanism. Independent targets must share a real enforceable host authority where
physical resources overlap; separate fixture hosts do not qualify that capacity
boundary. The Runtime kernel remains a single-principal component.

## Generic pinned admission

A trusted embedding can obtain `RuntimeInstance::compilation_snapshot()` and
compile the actual ExecuteFlow submission against that resource-bound capability
snapshot. `submit_pinned` binds admission to the process incarnation, concrete
request digest and compiled `ExecutionPlan` fingerprint. Runtime recompiles through
its normal admission path and rechecks current policy, pins, input scope and
resources before useful work. Changed targets, requests or contracts reject;
duplicate receipts identify the same admitted Run.

These methods remain in-process seams, absent from `CallerFacade` and portable
control JSON. They do not authenticate a caller, grant host capacity, construct
ComfyWorkFlow JSON or translate generation compositions into Runtime work.
