# Generation composition bridge

Status: integration contract for #19; bridge implementation and deployed host
qualification are not supplied by this document. It follows the reviewed Phase
A/B authority map and the current Phase C implementation at
`3e8b04137b7510023cb1799aaacfbe3e9cf73771`. Generation's v3 static foundation
is a compiler/history delivery, not production composition execution.

## Actual implementation anchors

| Existing boundary | Bridge obligation |
| --- | --- |
| `Compiler::compile_submission`, `flamoris.submit/1`, `flamoris.workflow/0.1` | Lower to these implemented schemas; never submit the old conceptual draft syntax |
| `ExecutionPlan`, `CapabilityPin`, `fingerprint_capability`, `verify_plan_pins` | Retain Runtime's own compiler/profile/IR digest and exact registered contracts |
| `RuntimeInstance`, `AuthorizationContext`, `PolicySnapshot`, `AuthorizationGate` | Admission, dispatch/retry/resume and access use current authenticated local authority |
| `AdapterAuthority`, move-only `AdapterGrant`, `RegisteredCapabilityAdapter` | Portable input cannot construct dispatch authority; the grant is consumed at the registered boundary |
| `RegisteredProviderPort`, `BoundedProviderSink`, `AdapterOutcome` | Bounded external work with truthful handoff/outcome evidence; no raw service URL in IR |
| `HostAuthorityPort`, `HostEnvelope`, `ResourceTicket`, `RuntimeRegistration` | External host grants remain distinct from logical media admission and provider readiness |

These are current code names, not proposed replacement classes. The media-root
delegation record, internal operation receipt and adapter transport below are
new contracts still requiring implementation and acceptance. No new public MCP
capability name is registered by this specification.

## Ownership and identity

Generation owns exact media Definitions, profiles, closure and artifact validation,
automatic media attestation, public media job identity, managed inputs and outputs.
Runtime owns its Execution Plan, Run, Job/Continuation and dispatch/resource state.
GPU Node Manager or the configured enforceable host authority owns host transitions.
Studio owns editor revisions and authenticated product-user mapping; Agent owns
conversation/Agent identity. Neither a media manifest nor an Agent request grants
execution rights.

The bridge retains an immutable relation:

| Relation field | Meaning |
| --- | --- |
| Generation process incarnation and public job ID | One root reservation; restart boundary |
| Authenticated principal and isolated target instance | Trusted deployment mapping, absent from portable Definition authority |
| Exact root and closure identities | Generation canonical definition/closure domains |
| Structural media-plan and bound-invocation digests | Qualification topology versus concrete input identities |
| Lowering revision and Runtime instance/Run/plan fingerprint | Runtime's independently compiled contract and process lifetime |
| Occurrence path, Runtime Job/attempt/dispatch identity and provider operation key | Exact stage provenance and at-most-once local handoff correlation |
| Input snapshots, provider evidence vector and host grant/epoch receipts | Current input and physical execution authority |

Occurrence paths identify repeated executions of the same immutable definition;
they are not collapsed into one Job. Credentials, private locators, live bearer
grants and provider filesystem paths never enter the portable manifest or public
observation stream.

## Root admission and internal provider operations

1. Generation validates the exact immutable closure, allowed profile/domain,
   current input ownership/expiry and attestation/infrastructure gates. It claims
   the ordinary public JobStore reservation and journals delegation preparation
   **before** any Runtime handoff. A failure proven before handoff releases the
   ordinary reservation; uncertain acceptance retains it.
2. Under that root, trusted Generation creates a bounded internal delegation
   record with target Runtime incarnation, principal, closure/invocation pins,
   allowed occurrence operations, cumulative ceilings and expiry. Its secret
   transport nonce is not an IR-authored authorization field. A portable root ID
   alone cannot retrieve or consume this record.
3. The bridge lowers only approved registered operations into the actual Runtime
   schema. Runtime compiles and admits with its own current authorization and
   records the returned Run identity. Loss of the admission response is uncertain
   delegation, never permission to submit the same plan again after restart.
4. Runtime's owning actor issues `AdapterGrant` only at a valid dispatch turn.
   The configured internal Generation port authenticates that trusted service
   target and resolves the matching delegation/occurrence. It rechecks current
   root scope, pins, budgets, expiry, provider evidence and concrete inputs.
5. Internal provider execution bypasses **only public root reservation acquisition**.
   It retains existing Generation validation, adapter isolation, staging leases,
   output publication and provider uncertainty rules. It cannot create a second
   root, select an arbitrary workflow, bypass another active root, or switch a GPU
   runtime. Operation receipt claims are atomic before provider handoff.

Duplicate transport delivery with the same scoped operation key/input digest
returns the existing operation observation; a changed digest conflicts. This is
process-lifetime ownership, not a provider deduplication or cross-crash exactly-once
guarantee. If durable state records a possibly accepted operation, restart performs
bounded read-only reconciliation and fences reuse; it does not restore Runtime Jobs.

The delegated capability set excludes public Generation root-submit operations,
and the trusted internal port rejects any root-admission purpose. Thus a
Generation-root -> Runtime -> Generation-root cycle rejects before acquiring a
second public reservation. A separate Runtime-originated media request may use
the ordinary public Generation boundary only when it carries no delegated root
ownership. A caller-authored depth/origin label cannot bypass this distinction.

## Baseline principal isolation

The first accepted deployment is a fixed single principal per authenticated
target `RuntimeInstance`, with per-principal embedding/process isolation. A shared
Studio gateway routes users only through a trusted principal-to-target mapping.
The gateway authenticates principal ownership on submit/status/result/events,
cancel, input and output access; arbitrary `user_id` JSON or a shared backend API
key does not establish it. Unknown/unmapped principals reject.

Each target has isolated submission indexes, Run observations, policy, adapter
operation records and managed-handle scope. A deployment sharing one process still
requires a reviewed trusted embedding boundary and cannot advertise kernel tenant
isolation from the presence of `AuthorizationContext.subject`. Different isolated
instances must use the same enforceable host capacity authority; separate local
ledgers cannot each grant the whole physical device. The initial Generation root
reservation may serialize users; fairness/concurrency expansion is separate work.

Revocation fences useful dispatch and data access while preserving bounded internal
stop/release/reconciliation authority. No cleanup action may start new media work.

## Lowering and complete limits

Compilation preserves exact provider/capability versions, schemas, adapter revision,
effects, resource contract digest, native pins and handle-validator revision through
`CapabilityContract` fingerprints. Include closure/profile/provider evidence and
Generation compiler pins remain a separate vector. Revalidate both vectors before
Runtime admission and each stage handoff/retry/resume. Changed evidence fails closed;
an unavailable stage never switches to another healthy provider.

Runtime's actual defaults include 60,000 ms timeout, 64 Jobs, 128 attempts, four
logical parallel workers and 1 MiB output JSON per Run. A 256-step Generation
ceiling does not override them. Count Runtime coordination/group Jobs, attempts,
mandatory events, input/result/control bytes and retained-state resources, not
only media leaf count. Effective limits are the intersection of Generation's
ceilings, the selected Runtime compiler profile and fresh host grants. Longer Music
deadlines require separately reviewed finite profiles; default timeout cannot be
silently enlarged to make a provider appear supported.

Runtime JSON stores numeric values as binary64. Media integers requiring exact
values above 2^53 (for example uint64 seeds) require a reviewed string/handle
encoding in the registered adapter schema, or reject at lowering. Do not round
Generation identities or parameters through a generic JSON number conversion.

Media bytes stay in Generation storage. Runtime receives bounded schema-qualified
immutable service handles with trusted `service_handle_type`/handle-validator
registration, owner/object scope, digest, MIME/role, size and expiry. The registered
local validator performs bounded validation; content reads/staging are explicit
effectful operations, never hidden validator I/O. Every intermediate is validated
for actual format, cardinality, downstream qualified domain and current access
before dependent dispatch. Missing or invalid required outputs stop dependents.

## Resource accounting and runtime evidence

The Generation public reservation is logical admission/exclusivity, not an extra
physical GPU reservation. Runtime has the single local physical ledger and an
enforceable `HostEnvelope`/acquisition acknowledgement for the real provider work.
Provider-owned memory, retained state, staging/result headroom and unknown outcomes
are represented once through reviewed conservative resource contracts. Shared
physical allocations have one identity; each Run still checks its working-set quota.
If host/provider overlap cannot be bounded and reconciled, dispatch is unavailable.

An advisory envelope, provider health response or completed activation request is
not enforcement. Current GPU Node Manager integration must separately prove
host epoch, ownership/capacity arbitration, release and reconciliation contracts;
this document does not claim those live receipts exist. Generation never runs
systemd or provider-local GPU activation as a shortcut.

Evidence guards are acquired in deterministic provider/capability order and retained
through input staging and provider acceptance. Image keeps its existing exclusive
mutation-lock continuity guarantees. Other adapters require reviewed equivalent
evidence. Mutually exclusive GPU stages stay unavailable until a separately
accepted bounded host stage-transition contract requalifies changed epochs.

## Observation, cancellation and finalization

| Runtime observation | Generation projection / reservation obligation |
| --- | --- |
| Valid preparation/admission/queued work | Existing root remains submitting/queued; no second root |
| Active authorized stage | Running with bounded occurrence-path progress; no invented percentage |
| Cancel accepted or deadline won | Close useful dispatch, request targeted stop, retain root/input leases until actual settlement |
| Run succeeded and every required media output validated/published | Complete public result after confirmed settlement of all root obligations |
| Definite failure and confirmed provider/host settlement | Fail public result and release ordinary reservation |
| Lost handoff response, unresolved external work or cleanup debt | Report unknown/incomplete ownership and retain affected reservation/capacity; do not infer successful cancellation |

Runtime `finalizing` settles its owned children/resources or explicitly transfers
contained debt before its terminal event. Generation must also receive the root's
provider/input/publication settlement receipts. A terminal Runtime failure with
`outcome_unknown` is not evidence that Generation's unknown public reservation is
safe to release. A bounded local cleanup timeout does not prove physical release.

Public success requires the complete declared required output set. Partial outputs
have explicit incomplete provenance and cannot supply successful downstream values.
Provider-original deletion is not implied by Generation snapshot cleanup. Cleanup
uses separately admitted finite deadlines/operations and reference-aware input/output
retention; debt remains accounted after useful-work cancellation or terminal status.

Once a lifecycle outcome is committed, late/replayed events update bounded authorized
observations only. Runtime's per-Run sequence/commit groups and snapshot watermark
are preserved; gaps are explicit. No result injection, provider call, resource release
or new submission can be triggered by replay. After restart, use the retained mapping
only to reconcile the old operation/host incarnation; no active Run is fabricated.

## Acceptance and implementation sequence

The additional B01–B14 cases in [Design Acceptance](DESIGN_ACCEPTANCE.md) are design
obligations, not passed tests. Implement reviewed slices in this order:

1. Exact media-to-Runtime lowering, bounded handle schemas and offline compiler cases.
2. Authenticated single-principal target and process-owned root delegation records;
   internal registered operation port with once-only claims and recursion rejection.
3. Current evidence/resource checks and cancellation/finalization/observation mapping.
4. Deterministic fixture acceptance, then real host/provider receipts and two-user
   deployment qualification before shared Studio use.

No native-kernel redesign, generic Generation scheduler, durable Run recovery or
production Maidionis/Arbitrium qualification is included. Those components can later
participate as bounded registered inference capabilities; they never execute the
media workflow themselves.

## CI image review prerequisite

The first bridge-document CI attempt stopped before compilation because hosted
Linux/Windows images now report `20260927.320.1`, while the repository required
`20260920.314.1`. The published exact image inventories were checked:

- [Ubuntu image inventory](https://github.com/actions/runner-images/blob/ubuntu24/20260927.320/images/ubuntu/Ubuntu2404-Readme.md)
  retains OpenSSL `3.0.13-0ubuntu3.15` and Clang/format/tidy `18.1.3`.
- [Windows image inventory](https://github.com/actions/runner-images/blob/win22/20260927.320/images/windows/Windows2022-Readme.md)
  retains OpenSSL `3.6.4` and Visual Studio `17.14.37710.0`.

The workflow and dependency image identities move together to that reviewed
revision. Existing exact compiler/package/hash checks, sanitizer lanes and full
build/tests remain enabled. Updated-image CI qualification is required before
merge; this does not weaken dependency pins or certify a physical provider/host.
