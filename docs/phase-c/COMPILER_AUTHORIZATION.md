# Compiler, authorization, and submission implementation

The production compiler uses the pinned nlohmann JSON 3.12.0 SAX parser with
owned values and early bounds. It accepts the reviewed closed
`flamoris.submit/1` / `flamoris.workflow/0.1` grammar, checks duplicate decoded
keys, encoding, numeric range/underflow, declarations, references, DAGs, scoped
groups, effect conflicts, capability pins, retry envelopes and finite limits.
The separate structural identity pass runs before a keyed submission claim;
mutable registry/profile resolution happens only for the admission owner.

Canonical semantic exports use UTF-16 key ordering, shortest binary64 spelling
with ECMAScript fixed/exponent thresholds, JSON string escaping, and OpenSSL EVP
SHA-256. Request, plan, capability and concrete operation-input digests have
separate domains. Plans omit display metadata, live inputs, authorization and
current availability. Direct inference lifts values into input slots and uses
the same compiled plan representation. Registered scope metadata and service
handle validator revisions participate in capability pins. Handle wire shape
remains registered schema; a trusted bounded local validator supplies current
owner, object scope and expiry, without hidden I/O or materialization.

`AuthorizationGate` revalidates current registry pins, availability, context,
capability/effects, concrete schema, object scopes, confirmation input digest,
deadline, cancellation, resource and preserved-state fences. It grants no
independent dispatch authority: the owning serialized control executor must
perform checks and commitment in one turn. Observation surfaces have independent
current access checks. `RunBudget` commits an entire cumulative charge or none;
cancellation does not refund attempted work. Retry evidence requires a stopped
prior attempt, finite attempts/original deadline, current guards and safe
outcome/provider deduplication evidence.

`SubmissionIndex` owns finite pending and response records. Absent keys allocate
fresh pending/Run IDs and never consult the keyed index. An explicit key binds
subject, request kind and canonical digest. Same-digest waiters receive the same
immutable decision; conflicts cannot replace the owner. Rejection fills all
local response slots before releasing the claim. Admitted claims survive terminal
failure until retention expiry. Disconnect releases a receipt only. Expiry and a
new process incarnation allow fresh admission without claiming non-execution or
cross-restart exactly once.

The default compiler profile uses 64 Jobs, 128 attempts and 64 resource operations
so the reviewed conservative mandatory event reservation fits the 64 MiB ceiling.
A deployment can explicitly supply another finite profile; requested values can
only tighten it. Group and dynamic child maxima share the Run allowance.

Component evidence is in `tests/unit/compiler_tests.cpp` and
`tests/unit/authorization_tests.cpp`. Acceptance tags identify the named boundary,
not completion of every cross-component scenario. Compiler tests perform no
resource, worker, transport or adapter calls. Integration, lifecycle cleanup,
external handoff and real native qualification require their respective suites.

Targeted commands after the normal CMake configuration are:

```sh
cmake --build build/gcc-debug --target unit_compiler_tests_cpp unit_authorization_tests_cpp -j2
ctest --test-dir build/gcc-debug -R 'unit_(compiler|authorization)_tests_cpp' --output-on-failure
```
