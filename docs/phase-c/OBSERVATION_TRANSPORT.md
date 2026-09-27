# Observation and caller projection

The C09/C11 implementation uses the existing C++ controller and resource ledger.
`EventStore` reserves complete groups before the controller changes state. It
publishes prepared owned values without allocation or subscriber acknowledgement.
Run identity, transition order and sequence watermarks survive retirement of all
retained groups. Publication values are bound to the store that prepared them.

Normal control, emergency stop/cleanup, optional telemetry and reconciliation
have separate finite slot bounds. A cleanup transfer reserves one observed record
and one closure record before it can support terminal publication. Whole groups
retire together. Cursor gaps, page bounds that cannot fit a whole group, expired
streams, subscriber overflow and telemetry loss are explicit observation metadata.
Subscribers retain only bounded cursor descriptors; they cannot block commitment.

`ControllerCleanupObservationPort` connects the actual resource ledger to the
controller's same-Run observation stream. It prepares two group templates at
reservation and uses a synchronous Run-ID lookup for later evidence. It retains
no controller pointer. Matching release can append after terminal; closure/expiry
never alters terminal intent, and ledger cleanup continues without reopening an
expired stream. An unexpected reserved publication failure sets the bridge's
`healthy()` flag false so the composition root can fail closed.
Closed bridge records are recycled before a later reservation. Tickets increase
monotonically, so a delayed callback cannot match a recycled slot.

`AuthorizedObservation` checks current event, export or replay scope on every
call. `ReplayProjection` accepts inspection values and has no execution, adapter,
resource or cleanup ports. It applies only complete supported groups, preserves
original identities, detects gaps and contradictory terminal updates, and reports
incomplete/redacted/unsupported input. It cannot restore live work.

## JSON-lines binding

`CallerFacade` and `serve_json_lines` implement the repository-owned
`flamoris.control/1` stdin/stdout contract. This is not an MCP server or an MCP
compatibility claim. The binding uses C++ standard streams and the same bounded
JSON parser/canonical writer already selected for C05; it adds no protocol
dependency. The authenticated `AuthorizationContext` comes from the composition
root, never from JSON. The root still applies current runtime authorization.

Every request is one UTF-8 JSON object on one line, at most 1 MiB before parsing.
Unknown keys, methods and versions reject. An oversized line is drained with a
bounded buffer and receives one rejection. EOF and write failure never imply
cancellation. Responses are objects with `schema_version:"flamoris.response/1"`,
`ok`, and exactly `value` or the bounded Runtime `error` projection.

| Method | Required fields besides `schema_version`, `method` | Optional fields |
| --- | --- | --- |
| `run.submit` | `submission`: exact `flamoris.submit/1` object | None |
| `run.status`, `run.result`, `run.cancel` | `run_id` | None |
| `run.pause`, `run.resume` | `run_id`, `command_id` | None |
| `events.read` | `run_id`, `after` | `limit`, default 256 and maximum 256 |
| `trace.replay` | `run_id` | None |

`run_id` is the opaque string `r.<instance-high>.<instance-low>.<counter>`.
Each component and every sequence/command/limit field above uses canonical
checked unsigned decimal text, preserving all uint64 values. Consumers must not
infer permission or future identities from this spelling. Request example:

```json
{"schema_version":"flamoris.control/1","method":"events.read","run_id":"r.7.9.1","after":"0","limit":"64"}
```

Status includes a watermark and bounded Job projections. Cancel/pause/resume
responses keep accepted, applied and terminal separate. Event envelopes include
the original group boundary and per-Run order. Error serialization accepts only
the closed Runtime error value, never upstream exception text.
Applicable attempt and command IDs remain envelope fields; dispatch generation
and external outcome remain payload fields. Their counters use decimal strings.

## Registered capability boundary

`AdapterAuthority` applies the current authorization gate and binds a move-only
grant to the concrete inputs, subject, capability pin, dispatch ticket, operation
key and deadline. A retry requires explicit `RetryEvidence`; a new ticket alone
cannot repeat a prior semantic operation. The adapter validates the binding and
consumes one bounded transcript entry before provider handoff.

`RegisteredProviderPort` is configured outside portable requests. It accepts no
endpoint, credential or executable command selection from the workflow. Its
worker receives through `BoundedProviderSink`, which enforces the result byte
limit while chunks arrive. Parsing and registered schema validation precede
binding. Oversized or invalid output preserves confirmed write evidence and
cannot trigger an automatic retry. A thrown/lost response after handoff remains
unknown unless the provider supplied stronger outcome evidence. Reconciliation
has a separate current grant and finite query budget; it never invokes the write.
Only explicit provider evidence of a transient, stopped, confirmed no-effect
attempt can make a failure eligible for the current retry policy. Error codes
alone cannot supply that evidence.

`PersistentAdapterBinding` owns one adapter transcript per configured capability
and serializes its provider workers. The actor never waits for its mutex. The
worker checks grant expiry after acquiring the mutex. A monotonic Run floor can
retire settled transcript entries; it must include pending reserved Run IDs as
well as admitted Runs. Unknown operations remain retained, and a grant below the
floor or from another runtime incarnation cannot invoke an operation again.

`IdentityProvider` is the implemented local pure capability. Tests use faulting
providers only at the external provider seam to cover unknown writes, schema
rejection, output overflow and duplicate handoffs. These tests do not establish
support for any deployed remote service.

## Evidence

`tests/observation/observation_tests.cpp` tests the production EventStore,
ReplayProjection and current access gate. `cleanup_observation_tests.cpp` joins
the production ResourceManager, RunController and cleanup observation bridge.
`tests/adapters/registered_adapter_tests.cpp` tests the production adapter with
controlled provider response loss and streaming failures. `facade_tests.cpp`
tests only JSON projection at its command-port seam; production Runtime tests
establish compiler/admission/dispatch integration separately.
`runtime_concurrency_tests.cpp` exercises pending duplicate ownership, shared
rejection, independent unkeyed work, replay port counts and shutdown through the
production Runtime. `runtime_crash_tests.cpp` discards actual component owners
without shutdown, retains host/provider evidence across incarnations and checks
that replay cannot manufacture execution or release evidence.
