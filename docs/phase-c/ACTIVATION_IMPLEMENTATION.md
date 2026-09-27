# Process activation and native residency integration

The default deployment is always-on: its host starts the process and constructs a
`RuntimeInstance`. Kernel admission does not launch services or eagerly load weights.
`UnavailableHostAuthority` rejects device work until a separately qualified host
binding provides an enforceable envelope and acquisition receipts.

For configured on-demand routing, `RuntimeActivationClient` is an outer caller-side
client. It owns an `ActivationClient` and injected host-gateway/policy ports. The host
gateway remains the owner of process starts, compatible waiter deduplication, process
resources, startup deadlines and containment. The client does not substitute a local
process table or service-manager command for that authority.

A trigger and its waiter identity do not create a Run. Only a matching `Ready` receipt
can attach a live Runtime endpoint. Attachment verifies the Runtime incarnation and
its current admission epoch. Endpoint references are weak, so a late host callback
cannot dereference or prolong a destroyed Runtime. Current inspect, model and stop
permissions remain separate; every actual submission also passes the Runtime's normal
compiler, submission identity, authorization, finite-limit and resource paths.

Readiness does not create a host resource grant. The Runtime can be ready while affected
device work remains unavailable. Readiness/epoch loss removes the outer route and fences
the attached Runtime's admission. There is no automatic resubmit, reconstruction of an
old Run or durable exactly-once claim after process replacement.

Idle stopping calls the actual Runtime `prepare_idle_stop(expected_admission_epoch)`
before requesting host drain. The actor-owned `AdmissionGate` orders pending submission
owners against that closure. A pending or admitted active Run prevents idle closure;
closure first prevents subsequent admission. Explicit shutdown uses `begin_drain`,
which closes new admission without asserting idleness or physical quiescence. Stop
permission is checked before gate closure and revalidated before the host request.
The old instance does not silently reopen when a subsequent caller arrives.

Native model loading is still work by an admitted existing Job. `RuntimeResidencyPool`
composes `ModelLoadRegistry`, immutable `TinyModel` holders and the physical allocation
ledger. A cold model has one loading Job; other eligible Jobs wait without obtaining
leases for duplicate loads. The owning worker's successful load receipt transfers an
immutable holder into the accounted residency pool after materialization. Subsequent
sessions receive that same holder and reserve only their incremental state/working
space, while their Run quota includes the shared model requirement.

Mutable inference state belongs to each native session. Each session's actual release
precedes dropping its logical model reference. The pool drops its own last holder only
when no session reference remains. An independent weak-pointer expiry check establishes
that the immutable model allocation has actually lost all physical owners before the
ledger accepts release. A surviving holder, stale epoch or uncertain loader stays
accounted and unavailable for replacement; logical Job completion is never an unload
receipt. Caller-supplied preloaded model holders are rejected by native registration.

`runtime_activation_tests.cpp` uses the actual Runtime actor with a deterministic external
gateway and host port: shared startup waiters, readiness before resource authority,
real submission deduplication, idle/admission ordering, permission rejection, host-epoch
fencing and callbacks after Runtime destruction. `runtime_residency_tests.cpp` uses real
native sessions and the resource ledger to verify one shared physical model, independent
state, first/last user release and unresolved external holder lifetime. Full Runtime
native-session and pause integration has its own acceptance tests.

These deterministic ports do not qualify a deployed host manager. Real on-demand
activation remains disabled unless an injected external gateway satisfies startup,
stop, arbitration and reconciliation conformance. No live host probing or service
changes are required by these tests.
