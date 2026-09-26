# Phase A Design Acceptance

## Status and method

**Review scenarios, not executed tests. No Runtime code exists.** These cases turn architecture claims into observable obligations. Phase B must map them to types, state commit points, fake clocks/backends/adapters and test seams; Phase C implements the tests. Passing a Markdown check is not passing these scenarios.

Read with [Execution Model](EXECUTION_MODEL.md), [State Machines](STATE_MACHINES.md), [Resource Model](RESOURCE_MODEL.md), [Authorization Model](AUTHORIZATION_MODEL.md), [Event Model](EVENT_MODEL.md), and [Failure Model](FAILURE_MODEL.md). Every case requires bounded, redacted event evidence and one lifecycle authority.

## Cross-component acceptance matrix

| ID | Stimulus / competing actions | Required observable outcome |
| --- | --- | --- |
| A01 | Compile identical IR/snapshot/compiler/limits twice | Same normalized semantics/fingerprint or same rejection; no dispatch or authorization granted |
| A02 | Missing binding, incompatible schema, reference-derived cycle, excessive expansion | Reject before execution; no partial graph or hidden reference I/O |
| A03 | Cache plan, then change a pinned effect/schema/adapter contract | `plan_stale` before dispatch/resume; no silent substitution or live migration |
| A04 | Revoke permission after compile/admission but before dispatch | Current check denies the operation; no effect handoff; cleanup authority retained |
| A05 | Two parallel paid children compete for the remaining budget | Atomic cumulative reservation permits only the fitting set; no per-Job overspend |
| A06 | Effect sets empty, unknown, pure+write, destructive without write | Deterministic rejection; composition of valid pure + external/paid drops pure from summary |
| A07 | Model proposes a new endpoint, out-of-scope capability, recursive child or repeated invalid call | No new authority; bounded rejection/recovery within original count/depth/deadline budgets |
| A08 | Inference yields, child succeeds, parent waits for capacity | Same Job ID; Continuation consumed once into queued payload; no execution lease while queued; retained allocation still charged |
| A09 | Duplicate/late child completion arrives after resume/cancel | Owner/suspension/attempt generations reject stale mutation; no second dispatch or result injection |
| A10 | Parent KV plus child working set cannot fit | Supported approved offload with target headroom, or explicit resource_deadlock failure; no indefinite wait or fake VRAM release |
| A11 | Two Jobs share a model; one releases it | One physical allocation; remaining live reference prevents eviction; per-Run quotas still checked |
| A12 | Offload copy succeeds but source release acknowledgement is lost | Both actual/uncertain footprints accounted; no successful evacuation or replacement admission claim |
| A13 | Old allocation-release callback arrives after host/backend generation changes | Reject stale mutation and reconcile the old record; fence dispatch only while current resource authority is unknown or unreconciled |
| A14 | Cancel and valid completion compete | First eligible commit selects stop or finalizing intent; later callback cannot replace it; workload deadline checked at acceptance |
| A15 | Completion arrives at/after deadline before timer callback | Timeout wins; suspension/queue/retry never resets deadline; cleanup uses separate bounded allowance |
| A16 | Pause parent while child is running; child completes | Child may settle; parent keeps satisfied wait state without auto-resume; no fabricated Run-wide paused claim |
| A17 | Run pause includes an opaque operation without safe pause | Reject unsupported barrier without partially applying a successful pause; accepted barriers report pending until quiescent |
| A18 | Pause barrier times out after some Jobs pause | Clear only that command's pause flags; preserve prior targeted pauses and state; normal resume authorization still applies |
| A19 | Resume with invalid preserved state or revoked permission | Fail and account cleanup; never recreate inference and call it equivalent resume |
| A20 | Required child times out during all_success | Fail required coordination, stop unfinished siblings, invalidate parent resume, settle subtree before terminal |
| A21 | all_settled has successes and failures | Wait for all terminal outcomes; bounded typed collection in declared order; group deadline still applies |
| A22 | Race candidates finish together or first candidate fails acceptance | Run commit order/declaration tie-break chooses one accepted success; failed/unacceptable result cannot win |
| A23 | Race winner selected while remote loser will not confirm stop | Winner immutable; loser outcome/cost/quarantine explicit; loser terminal before parent terminal, no rollback claim |
| A24 | Race requests writes or more paid attempts than aggregate budget permits | Reject initial write/destructive race or budget violation before any participant dispatch |
| A25 | Provider accepts write but response is lost | Unknown outcome; no blind retry; reconcile under a bounded registered contract |
| A26 | Provider write completes but output is invalid/oversized | Fail result validation, preserve effect evidence, never inject rejected data or repeat write to obtain output |
| A27 | Same submission key with same/different digest; key expires or process restarts | Same Run/conflict inside retention; no cross-restart exactly-once claim and no absence-as-nonexecution inference |
| A28 | Child or native backend cannot stop before cleanup deadline | No fictitious terminal/quiescence; only proven safe containment permits transfer; uncertain capacity stays unavailable |
| A29 | Terminal Job has quarantined resource and later matching release ack | Reconcile cleanup ledger once, no state resurrection, no duplicate accounting release |
| A30 | Slow observer, token flood, exhausted normal event capacity | Stop new admission as needed; reserved control/cleanup capacity remains; bounded telemetry loss/gaps explicit |
| A31 | Reconnect cursor expired or persisted event group incomplete | Return explicit gap/current snapshot watermark; no invented complete history |
| A32 | Replay trace containing paid/write/cancel/resource events | Only isolated observation changes; zero backend, adapter, scheduler or cleanup dispatch |
| A33 | Crash immediately before/after external handoff | Trace may be incomplete; new instance does not resume or infer outcome; reconcile host/provider before reuse |
| A34 | Unbounded stream of warm-model Jobs competes with older cold work | Fairness bound prevents indefinite bypass; dependencies/effect ordering never relaxed for residency |
| A35 | Another principal requests status, media handle, trace or replay | Current scope checked for each surface; no cross-tenant/expired-handle leakage |
| A36 | Two unordered writes may target the same object | Reject ambiguous ordering unless trusted contract proves disjoint scopes; physical serialization is not semantic order |
| A37 | Targeted pause quiesces every remaining Job without a Run barrier | Defined activity projection with pause reason; Run.resume never clears unrelated targeted pauses |
| A38 | Deadline/cancel arrives after success entered finalizing | Frozen result intent remains; bounded cleanup continues and residue is separately reported |

## Worked failure trace: inference awaits an MCP child

Assume an admitted plan allows one required child, the backend advertises safe preservation, no typed recovery branch is selected, and the child deadline is earlier than the parent deadline. Model state remains resident. This example is a proposed observable contract, not a live host trace.

| Order | Committed fact / observation | Ownership and resource obligation |
| --- | --- | --- |
| 1 | Parent proposes child work, then backend confirms a valid quiescent safe point | Bound inputs/capability and cumulative budgets; validate preserved-state and quiescence evidence before suspension |
| 2 | Register child and create parent Continuation with `running -> waiting` in the ordered control commit | Same parent Job; child cannot dispatch before registration and suspension |
| 3 | Release parent execution capacity using the already confirmed quiescence | Retained model/KV bytes remain allocated; child must fit actual remaining capacity |
| 4 | Child passes current dispatch checks; adapter handoff intent is committed | Child owns attempt and external operation reference; intent is not provider success |
| 5 | Child deadline expires without authoritative outcome | Close child dispatch, request stop, keep primary timeout plus external unknown evidence |
| 6 | Child reaches safe local containment and finalizes failed | Quarantine uncertain remote operation/cost; no blind retry and no fictitious rollback |
| 7 | Required wait fails; parent enters cancellation/failure cleanup | Discard Continuation resumability, cancel other required work, never inject success |
| 8 | Parent backend state release is confirmed or safely transferred | Account retained bytes until acknowledgement/containment; immutable IDs/generations prevent late wake |
| 9 | Children terminal, parent finalizing completes, then Run terminal | Root failure retains causal child timeout and cleanup/outcome evidence |

Independent cleanup acknowledgements may arrive in different orders. Quiescence acknowledgement before suspension is a required gate and cannot be reordered. The Kernel cannot fabricate an acknowledgement to make the table advance. If in-process execution cannot be safely contained, A28 applies and terminal publication waits for real containment/supervisor action.

## Phase A review checklist

- [ ] Independent review confirms no conflicting lifecycle/resource/authorization authority.
- [ ] Every matrix outcome is supported by a detailed contract; missing semantics block Phase A acceptance.
- [ ] All external integration and backend feasibility assumptions remain explicit Phase B research obligations.
- [ ] Exact wire/C++ interfaces, build/test commands and implemented status are not invented.
- [ ] Phase B preserves the accepted semantics or records a reviewed deviation with replacement cases.

Unchecked boxes are the reviewer gate, not a claim that executable tests ran. The proposal is ready for review when its document consistency and scope checks pass; review/merge is still separate from implementation.
