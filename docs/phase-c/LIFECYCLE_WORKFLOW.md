# Lifecycle, scheduling and workflow implementation

The implementation lives in `lifecycle.hpp`, `scheduler.hpp`, `workflow.hpp` and
their `src/kernel` translation units. The public source interfaces are internal
composition contracts, not a stable ABI or caller authorization surface.

`RunController` owns the Job tree and is the sole lifecycle committer. A prepared
complete event group reserves EventStore capacity before state mutation; state,
move-only continuation transfer, the Run activity projection and the group become
visible in one serialized control turn. Failed preparation changes neither the
Job nor its event watermark. Ordinary control capacity is separate from the
reserved stopping/finalization allowance. Results are observations; they cannot
dispatch another Job by being replayed.

Suspension preserves Job and attempt identity. A continuation is consumed into
the same Job's pending resume payload once, with monotonic suspension and dispatch
generations. Temporary capacity denial leaves the payload owned and retained.
Resume requires a fresh dispatch check. Actual native state and allocation
ownership belong to the native worker and resource ledger; descriptor movement
does not assert physical release. The composition root supplies matched current
resource/policy/native evidence to the controller's internal checks.

Targeted pauses and Run barriers have independent causes. Run pause validates the
whole current subtree and closes child registration and dispatch together.
Unsupported targets reject without changing those gates. Child results can satisfy
a paused wait without queuing its parent. A barrier timeout removes only that
barrier's causes. Accepted finalizing intent survives a later workload timeout or
cancel; a separate cleanup timeout reports unresolved ownership and never invents
quiescence. Parent terminal publication requires all child Jobs terminal and
acknowledged release or an explicitly contained, prepaid cleanup transfer.

The Scheduler contains only bounded Job IDs and advisory resource requirements.
It orders eligible Jobs by the four reviewed priority classes, one-second aging,
readiness sequence and Job ID. A conflicting older Job is protected at three
seconds, and the acquisition deadline is clipped to five seconds and the original
Job deadline. Final current authorization and full-vector ledger admission remain
the composition root's dispatch obligations.

WorkflowMachine executes only immutable compiled steps. It freezes invocation
inputs before registering child Jobs, and registers each ready wave and suspends
its coordinator atomically. Every nested await/join/race has its own coordinating
Job and owns its members. Group inputs are explicit; member bindings cannot access
outer nodes. The current implementation advances topological waves; it does not
promise immediate dispatch of a downstream node while an independent member of
the current wave remains unfinished.

Each invocation retains its compiled attempt limit and backoff. A retry is a
controller transition on the existing Job only after current authorization and
explicit provider stop/outcome evidence pass `authorize_retry`; the original
deadline and provider operation key remain fixed. An error name alone never
supplies that evidence.

All-success propagates required failures and cancels remaining children.
All-settled preserves declared participant order and typed error envelopes. Race
acceptance evaluates only validated terminal successes; separate observations use
controller order and an explicitly simultaneous batch uses declared order. Winner
publication precedes loser cancellation and parent readiness. A losing child's
unknown external outcome remains its failure provenance and blocks parent terminal
publication until its cleanup authority settles.

Dynamic fragments use the same compiler and the admitted envelope's exact pins,
depth and cumulative budgets. `WorkflowMachine::create_at` attaches an already
compiled fragment to an explicitly registered coordinating child Job; it grants
no permission and cannot create a new Run. Native safe-point preservation and
input injection remain the native machine's responsibility.

Deterministic offline evidence is in `tests/acceptance/lifecycle_tests.cpp`,
`scheduler_tests.cpp` and `workflow_tests.cpp`. These tests call the production
compiler, controller, EventStore, Scheduler and WorkflowMachine. Fake clocks and
manually acknowledged native/resource evidence control external timing; a fake
lifecycle outcome does not replace the controller being tested.

The transition predicate is checked for every state pair. A separate operation
matrix builds each state through production controller histories and attempts
inapplicable queue, dispatch, suspension, wake, completion, stopped-observation,
finalize, retry and child-registration operations; rejected operations preserve
the snapshot and watermark. Guard-specific tests cover timeout equality, current
resume denial, preparation failure, pause barrier timeout and cleanup evidence.
Control-request rejection events are tested separately from rejected lifecycle
proposals, since an admitted unsupported command intentionally records rejection.

With the documented toolchain installed, run:

```sh
cmake --preset gcc-debug
cmake --build --preset gcc-debug
ctest --test-dir build/gcc-debug --output-on-failure -R 'acceptance_(lifecycle|scheduler|workflow)'
```

The integration/native lanes separately establish actual CPU and OpenCL execution
and release evidence. These deterministic coordination tests alone make no native
pause, compute correctness, physical release or external host availability claim.
