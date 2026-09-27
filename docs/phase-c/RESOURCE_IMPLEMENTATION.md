# Resource implementation and integration evidence

C03 implements one control-executor-owned `ResourceManager`. Its host and native
receipts are trusted integration values, not Workflow-authored claims. The live
ledger is independent of Run retention and never owns a physical device handle.

## Capacity and native materialization

For each logical resource, admission checks the complete vector:

`unique resident allocations + unmaterialized reservations + executing capacity`

A reservation is all-or-none. Host acquisition is separately acknowledged before
`commit_dispatch` grants a lease. A fresh envelope must name this Runtime
incarnation, an enforced arbitration mode, a verified contract, a host epoch and
reconciled inventory. A free-memory observation is insufficient.

Native implementations reserve conservative peak bounds. Their required initial
allocations can be smaller than those bounds because scratch and growth headroom
has not yet materialized. This implementation refines the Phase B phrase
“complete required vector is materialized” as follows:

1. `next_allocation_identity(ticket)` registers allocation identities before native
   work. Receipts may arrive in a different order.
2. `materialized` converts only the reported footprint from reserved to resident.
   It checks the reservation, incarnation, host epoch and native worker generation.
3. `materialization_complete(ticket, manifest, now)` accepts the native owner's
   complete initial allocation manifest. Every registered initial allocation must
   be accounted and no duplicate or foreign identity is accepted. This enables
   `ready_for_use` while any unused peak bound **remains charged as reservation**.
4. Later growth or scratch materialization consumes that remaining bound. Growth
   beyond it needs another admission. Completion of the manifest does not release
   headroom or pretend that reserved capacity is a physical allocation.
5. Genuine native quiescence with explicit absence evidence, together with the
   matching host release, releases the unmaterialized remainder. Retained model
   and state allocations remain charged until their own release receipts.

This distinction preserves the Phase A physical-accounting invariant and prevents
both a cold-load deadlock and artificial allocation records for unused estimates.
A load uses `lease_current`; useful execution uses `ready_for_use`. The manifest
is worker evidence, never a permission grant or a substitute for current policy.
Shared model storage is one physical allocation, while every referencing Run's
working set is checked against its own limit.

## Stop, release and uncertainty

`request_host_release(ticket, operation)` stores its exact operation identity
before invoking the injected host port. An exception after handoff retains that
identity and all liability. Repeating the same request returns its current local
status without issuing another release. `host_release_operation` exposes the
stored identity for a trusted adapter; it is not caller authorization.

`native_quiesced` records actual worker stop/containment and whether unmaterialized
storage is authoritatively absent. `observe_host_release` accepts only the matching
operation, ticket and grant. These two receipts may arrive in either order.
`reservation_settled` becomes true only after both obligations settle. Release
callbacks use ledger identity directly and do not require a retained Run or Driver.

A reservation whose external acquisition is unknown stays charged. Failure to
obtain a response is not `not_dispatched`. `ticket_for(operation)` recovers the
owned reservation after an ambiguous outbound call. `abort_preparation` requests
one bounded reconciliation operation and releases capacity only with affirmative
no-acquisition evidence; repeated control turns do not flood the port.

`transfer_to_cleanup` accounts known allocations. `transfer_acquisition_to_cleanup`
accounts an uncertain operation or native worker even when no physical allocation
receipt has arrived. Both require real worker quiescence or authoritative isolation,
and a reserved bounded observation/closure ticket before transfer. Callback fencing
alone is rejected. Transferred leases cannot start new work, and timeouts do not
refund quarantined bytes, reservations or concurrency.

## Retention and retirement

Cleanup observations have a finite window. They close as resolved or still unknown;
matching later receipts still reconcile the ledger without reopening that stream.
Terminal lifecycle intent is untouched.

`retire_settled` reclaims only fully acknowledged allocation/reservation records and
resolved, closed cleanup records. It never expires uncertain debt. Allocation IDs
have a monotonic high-water fence, so retired IDs cannot identify new storage.
Pre-registration permits legitimate out-of-order receipts. Old callback identities
cannot affect a new allocation, lease or Runtime instance.

## Verification boundary

`resources_tests.cpp` exercises manifest omissions, duplicates and stale epochs;
shared physical versus Run quota accounting; source/target transfer overlap;
unknown acquisition and throwing host handoffs; separate physical/lease release;
unsafe containment rejection; post-retention reconciliation; and repeated operation
cycles through small record limits. Runtime composition tests additionally exercise
these ports with the actual control executor and native/provider drivers.

`UnavailableHostAuthority` is the fail-closed default. The deterministic host port
is a test double, not evidence that a deployed GPU Node Manager implements this
protocol. Real host arbitration, device fencing, process activation and authoritative
reconciliation remain unavailable unless the configured external adapter is separately
qualified. No service commands, device resets or live infrastructure probes are used
as substitutes. A process restart needs a new reconciled envelope and never restores
Jobs, execution leases or permission from a trace.
