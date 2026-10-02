# Isolated principal Runtime target

`PrincipalRuntimeTarget` is a trusted embedding boundary for the B13 portion of
[the Generation composition bridge](../GENERATION_COMPOSITION_BRIDGE.md).
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

This is source-level target isolation, not deployed two-user qualification. The
Generation domain/network adapter, authenticated gateway mapping, process-owned
root delegation journal, atomic internal claims and result publication remain
under #19. Independent targets must share a real enforceable host authority where
physical resources overlap; separate fixture hosts do not qualify that capacity
boundary. The Runtime kernel remains a single-principal component.
