# Media-to-Runtime lowering

The first implementation slice of [the bridge contract](../GENERATION_COMPOSITION_BRIDGE.md)
is `lower_media_submission` in `media_lowering.hpp`. It is an in-process, pure
compiler boundary. A trusted Generation adapter must still supply domain-validated
operations and current media identities; no new public wire schema or capability
is registered here.

Each operation names an exact registered capability pin and an occurrence path.
Trusted adapter configuration separately approves internal provider operations;
public root-admission operations reject before compilation. Paths are sorted and
mapped to distinct Runtime node IDs. Bindings, explicit ordering and outputs lower
to the existing `flamoris.submit/1` and `flamoris.workflow/0.1` schemas, then pass
through the existing Runtime compiler. Optional defaults must already be resolved
by the owning media compiler. No stage retry or idempotency key is invented.

The compiler retains its original authority for schema compatibility, dependency
cycles, overlapping effect ordering, root Job/attempt counts, output ceilings and
mandatory event storage. Media effects must match the complete registered effect
aggregate. Every Run ceiling is explicit and must fit the selected compiler profile.
A required stage deadline must fit its exact provider and Run contract; it is never
silently rounded or enlarged. Unsafe binary64 integer values fail before admission.

Handle inputs and outputs use the registered bounded `ValueSchema` and trusted
handle-validator revision. Compilation preserves that validator provenance and
rejects literal handles; it never reads assets or calls the handle validator.
Runtime admission/dispatch remains responsible for current owner/expiry checks.

`LoweredMediaSubmission` keeps the original root/closure/structural/invocation/
evidence identities, occurrence mapping, Runtime plan fingerprint and concrete
submission digest. Its domain-separated relation digest ties those separate
identities together. It does not recompute Generation's media digests, attest a
provider, authenticate a principal or grant execution authority. Different concrete
input values change the submission/relation identity while an unchanged structural
plan retains its Runtime plan fingerprint.

## Executable evidence and remaining gates

`tests/unit/media_lowering_tests.cpp` covers deterministic identity/order, independent
occurrences, stale/missing/unavailable pins, recursion-purpose rejection, incomplete
effects, unordered writes, root/attempt/event/output/deadline bounds, unsafe integers,
invalid bindings/domains/cycles/hidden work and registered handle provenance.
These are offline compile tests for portions of B01/B02/B03/B04/B06/B07, not complete
bridge acceptance or deployed host/media evidence.

Still required: Generation manifest/domain adapter, authenticated isolated targets,
process-owned root delegations, atomic once-only internal provider handoff,
current evidence guards, shared enforceable host grants and settlement/publication
receipts. Cancellation/finalization/observation and two-user deployment acceptance
remain under #19. No provider, GPU, database or deployed service changes occur here.
