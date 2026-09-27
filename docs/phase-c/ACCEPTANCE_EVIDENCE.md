# Acceptance evidence index

This index maps reviewed obligations to executable source. It is not a test-run
report and does not turn a matching test label into evidence of every branch of
an acceptance case. The [status report](STATUS.md) records executed configurations
and remaining integration work. Phase A [semantics](../DESIGN_ACCEPTANCE.md) and
the Phase B [test design](../phase-b/ACCEPTANCE_TEST_MAP.md) remain authoritative.

Component tests inject timing, native or provider evidence only at the named
port. Tests exercise the production controller, compiler, resource ledger,
adapter or observation component under test. Real native arithmetic and deployed
host enforcement have separate qualification gates.

| Case | Reviewed obligation | Executable evidence boundary |
| --- | --- | --- |
| A01 | Deterministic compilation | [Compiler](../../tests/unit/compiler_tests.cpp): canonical identity, key order and semantic mutation |
| A02 | Malformed or excessive plans | Compiler: parser, schemas, references, cycles, expansion and finite bounds |
| A03 | Stale contract pins | Compiler and [authorization](../../tests/unit/authorization_tests.cpp): pinned contract changes versus current availability |
| A04 | Current authorization at handoff | Authorization, [registered adapter](../../tests/adapters/registered_adapter_tests.cpp) and [inference machine](../../tests/acceptance/inference_tests.cpp) |
| A05 | Shared finite Run allowance | Authorization: atomic cumulative reservations in both contention orders |
| A06 | Effect algebra | [Domain](../../tests/unit/domain_tests.cpp): exhaustive invalid masks and composition |
| A07 | Untrusted dynamic proposal envelope | Compiler, [workflow](../../tests/acceptance/workflow_tests.cpp), [lifecycle](../../tests/acceptance/lifecycle_tests.cpp) and inference machine |
| A08 | Same-Job yield and capacity wait | Lifecycle and inference machine: continuation transfer, queued preservation and fresh dispatch |
| A09 | Duplicate and late wakeups | Lifecycle, inference machine and [callback mailbox](../../tests/unit/control_mailbox_tests.cpp) |
| A10 | Retained-state dependency deadlock | [Resources](../../tests/unit/resources_tests.cpp) and [scheduler](../../tests/acceptance/scheduler_tests.cpp) |
| A11 | Shared model allocation and quotas | Resources: one physical allocation, separate working-set references and quotas |
| A12 | Copy without source release | Resources: independently retained source, target and staging accounting |
| A13 | Old epoch callback | Resources: full-identity fencing and independent replacement records |
| A14 | Cancellation versus completion | Lifecycle and inference machine: controller commitment order and suppressed late useful output |
| A15 | Deadline before timer delivery | Lifecycle and inference machine: monotonic dispatch/completion checks |
| A16 | Targeted parent pause | Lifecycle and workflow: child result cannot clear a targeted pause |
| A17 | Unsupported Run pause / pending barrier | Lifecycle: whole-subtree preflight and acknowledged barrier completion |
| A18 | Pause barrier timeout | Lifecycle: remove only the expired barrier's causes |
| A19 | Unusable or unauthorized resume | Lifecycle, authorization and inference machine: preserved-state and current-access checks |
| A20 | Required child timeout | Workflow and lifecycle: required failure propagation and owned subtree settlement |
| A21 | All-settled collection | Workflow: typed outcomes in declared participant order |
| A22 | Race selection | Workflow: valid terminal candidates and declared order for simultaneous batches |
| A23 | Fixed winner / uncertain loser | Workflow and resources: losing work remains owned and accounted |
| A24 | Forbidden or excessive race | Compiler: effects and aggregate limits before scheduling |
| A25 | Accepted write / lost response | Registered adapter and authorization: unknown outcome, separate reconciliation and retry refusal |
| A26 | Confirmed write / rejected result | Registered adapter: streamed byte bounds and schema rejection preserve effect evidence |
| A27 | Atomic submission claim | Authorization/submission, [Runtime integration](../../tests/acceptance/runtime_tests.cpp) and [barrier-controlled concurrency](../../tests/acceptance/runtime_concurrency_tests.cpp) |
| A28 | Inability to stop or contain | Lifecycle, inference machine and resources: no invented quiescence or release |
| A29 | Bounded late reconciliation | Resources and [cleanup observation integration](../../tests/observation/cleanup_observation_tests.cpp) |
| A30 | Bounded events / slow subscribers | [Observation](../../tests/observation/observation_tests.cpp), lifecycle and callback mailbox |
| A31 | Cursor gap / incomplete group | Observation: whole-group pagination and explicit cursor/overflow metadata |
| A32 | Observation-only replay | Observation and Runtime concurrency: separate projection, unchanged ledger and zero extra external calls |
| A33 | Process failure around handoff | [Abrupt component lifetime](../../tests/acceptance/runtime_crash_tests.cpp): external transcript survives lost in-memory owners; weak callbacks and full incarnation fencing |
| A34 | Bounded fairness | Scheduler: priority aging, conflict protection and fixed acquisition deadline |
| A35 | Current observation / handle access | Authorization and observation: separate surfaces, concrete scope and expiry |
| A36 | Conflicting unordered effects | Compiler: explicit dependencies or pinned disjointness proof |
| A37 | All Jobs targeted-paused | Lifecycle and scheduler: Run activity and independent pause causes |
| A38 | Control after successful finalizing | Lifecycle: immutable terminal intent and separate cleanup allowance |
| A39 | Direct inference shares compiler path | Compiler and Runtime integration |
| A40 | Child creation versus Run pause | Lifecycle and inference machine: atomic registration/barrier boundary |

A41–A42 and B-PAID02 belong to the optional strict-cost profile and are excluded
from this baseline. No durable recovery or cross-crash exactly-once guarantee is
derived from in-memory submission deduplication.

## Supplemental boundaries

| Obligation | Source |
| --- | --- |
| B-LIFE01 / B-EVENT01 | Lifecycle transition rules, complete prepared event groups and failed preparation |
| B-CALL01 / B-DRAIN01 | Callback mailbox bounds, reserved completion slots, close/drain fencing and weak receiver lifetime; Runtime shutdown is a separate composition check |
| B-RETRY01 | Authorization, lifecycle, registered adapter and [Runtime retry](../../tests/acceptance/runtime_retry_tests.cpp): explicit prior-attempt outcome, backoff, current guards and stable operation identity |
| B-INPUT01 | Inference machine and native session: bounded injection, pending token, RNG and output-state preservation |
| B-SER01 | Compiler and facade: closed grammar, numeric/encoding bounds and sanitized projections |
| B-ACT01–B-ACT12 | [Activation](../../tests/unit/activation_tests.cpp): production client/gate/model-load registry with explicit external receipts |
| B-NATIVE01 / B-REAL01–B-REAL07 | [Native qualification](../../fixtures/native/QUALIFICATION.md) and the real native worker suites |
| B-OPENCL01 | Actual device numerical and resource-lifetime suite, recorded separately from CPU and fake-worker evidence |
| B-HOST01 | Separate deployed cross-process host-enforcement gate; deterministic host ports do not establish it |

The integrated Runtime suites use the same production components with explicit
host acknowledgements. A host-grant callback is trusted deployment input, never a
permission read from portable Workflow JSON. Unsupported native profiles reject
instead of silently restarting inference or selecting an external backend.
