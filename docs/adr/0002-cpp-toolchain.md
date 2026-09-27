# ADR 0002: C++20, explicit ownership, and an offline-first build

Status: proposed for Phase B review, 2026-09-27. No build files or production
declarations are introduced by this ADR. Decisions below describe Phase C work.

## Context and decision

The Kernel needs move-only ownership, typed sums, bounded views, clocks and
ordinary synchronization. Select **C++20**, using `variant`, `optional`, `span`,
`chrono`, `unique_ptr`, and explicit message queues. Do not require modules,
coroutines, reflection, ranges-heavy APIs or `std::format`. In particular,
`std::expected` is not a C++20 dependency: the small internal `Result<T>` facade
specified in [Error/Serialization](../phase-b/ERROR_SERIALIZATION.md) uses a
discriminated value/error representation.

This is a conservative project baseline, not a claim of complete C++20 support
in every compiler. GCC, Clang and Microsoft publish feature-level conformance
tables [T1–T3]. The selected feature subset needs independent compilation in
each proposed CI lane before support is advertised. Existing private-foundation
code is research input; no private build conventions or transitive dependencies
are inherited without the separate reuse assessment.

Choose **CMake 3.28 or later**, with Ninja for Linux and an initialized MSVC
environment plus Ninja on Windows. Target-level C++20 requirements must be
mandatory, extensions off; configuration must reject an unsupported compiler
instead of lowering the language standard. Checked-in presets use schema 6,
which is supported by this baseline, and separate configure/build/test presets
[T4]. Developer-specific presets remain untracked.

## Intended build and ownership boundaries

| Proposed path / target | Responsibility | Dependency restriction |
| --- | --- | --- |
| `include/flamoris/runtime/` | Reviewed source-level consumer contracts and immutable values | No external runtime, HTTP or JSON library types in consumer signatures |
| `src/domain/`, `flamoris_runtime_domain` | IDs, validated effects, failure values, Job/Continuation state rules | Standard library only |
| `src/kernel/`, `flamoris_runtime_kernel` | Controllers, scheduler, resource ledger, admission and event commits | Domain and injected abstract ports; no model implementation |
| `src/compiler/`, `flamoris_runtime_compiler` | Bounded IR, normalization, immutable plan | Domain, private serialization/fingerprint implementation |
| `src/observation/`, `flamoris_runtime_observation` | Retained groups, snapshots, inspection-only replay | Domain and observation ports; no dispatch-capable adapter |
| `src/model/`, `src/compute/` | FLAMORIS native model execution and CPU/OpenCL operations | Kernel control contracts; no third-party runtime types |
| `src/adapters/`, opt-in adapter targets | Host and registered Workflow capability integrations; optional budget integration separately | Kernel ports; no lifecycle mutation shortcuts |
| `src/transports/`, `apps/` | Later MCP/API/CLI composition roots | Kernel source API; authenticate and submit commands only |
| `tests/unit/`, `tests/acceptance/`, `tests/support/` | Offline tests and deterministic fakes | No live service, GPU, weights or network |
| `tests/integration/` | Explicit opt-in native CPU/OpenCL and external capability checks | Never part of the offline contract |
| `cmake/`, `CMakePresets.json` | Target policies, pinned dependency manifest and presets | No machine-specific paths or implicit service setup |

Libraries are ordinary static targets initially; the graph may be combined for
packaging without changing authorities. Expose `flamoris::runtime` namespaces
with narrower `detail` implementation namespaces. No loadable plugin ABI,
public stable binary ABI, shared-library distribution, binding generator or
package publication is promised. No executable exists until a real composition
root has useful tested behavior; do not create a placeholder server in slice 1.

## Dependencies

Use standard-library facilities in domain/kernel code. The selected production
dependencies for the compiler boundary are **nlohmann/json 3.12.0** for bounded
JSON parsing and **OpenSSL 3.x libcrypto** through EVP for SHA-256; no TLS stack
is required by the domain. nlohmann exposes abortable SAX callbacks [T6], but
duplicate-key detection, byte/depth caps and canonicalization remain Runtime
responsibilities. Its ordinary `dump()` is not accepted as proof of RFC 8785
canonicalization. EVP is the upstream-recommended digest interface [T7]. Use a
currently security-maintained OpenSSL 3.x patch at implementation time; record
the exact version and package/source checksum in the lock manifest rather than
freezing an old security patch in a design ADR.

Select **Catch2 v3** (initial candidate v3.8.1) and its CMake/CTest integration
[T5] for tests. Assertions execute on the controlling test thread; worker
failures are captured as observations and checked after deterministic drain.
Fakes are small hand-written implementations of ports; no mocking framework is
needed. Exact source revisions, archive checksums, licenses/notices and build
options must be locked when each dependency is introduced. Researching a
release here does not assert that it has passed our build.

Dependency resolution is explicit provisioning, separate from configure/test.
CI may fetch a reviewed checksum-pinned archive or package first; ordinary
configuration consumes a local dependency cache and fails clearly if absent.
Do not silently download from `main`, download model weights, probe GPUs, or
contact services during configure/test. `find_package` may satisfy a dependency
only when its resolved version matches the reviewed manifest policy. Optional
optional native compute dependencies are disabled by default and have their own lock entries.
Security upgrades change the lock in a dedicated reviewed commit.

## Proposed CI support matrix

These are required **future lanes**, not checks run by this design PR. Pin the
runner/container image and resolved compiler patch in slice 1; reject accidental
toolchain changes instead of relying solely on a moving `latest` label.

| Lane | Baseline | Checks |
| --- | --- | --- |
| Linux x86-64 | GCC 13, libstdc++, Ubuntu 24.04 userspace | Clean configure, Debug and Release build, all offline tests |
| Linux x86-64 | Clang 18 with the tested libstdc++ baseline | Debug build, offline tests, ASan+UBSan, warnings/static analysis |
| Linux x86-64 race lane | Clang 18, separate TSan configuration | Real-thread mailbox/shutdown stress tests with deterministic barriers |
| Windows x64 | Visual Studio 2022 17.10 or newer reviewed 17.x, MSVC/STL | Clean Debug and Release build, all offline tests |
| Documentation | Existing repository checks plus Phase C local link checking | Links, forbidden implementation claims and policy conformance |

ARM, macOS and GPU toolchains are not initially supported merely because some
dependencies support them. CPU-only tests do not certify accelerator behavior.
Never combine TSan with ASan in one binary; sanitizer build/test lanes remain
separate. TSan is a dynamic race detector with platform limitations [T8], not a
proof that message ordering is correct. The manual-executor acceptance suite
is the semantic authority for race outcomes.

Format with pinned **clang-format 18** and a committed configuration (LLVM base,
four-space indentation, 100-column limit). Analyze project-owned code with pinned
**clang-tidy 18**, initially `clang-analyzer-*`, selected `bugprone-*`,
`performance-*`, and ownership checks reviewed against the contract [T9–T10].
Use GCC/Clang `-Wall -Wextra -Wpedantic` and MSVC `/W4 /permissive-`; warnings
are errors in the locked project CI lanes, not globally forced on dependencies
or downstream consumers. Every suppression names a concrete reason. Static
analysis supplements tests rather than redefining resource-release semantics.

## Slice 1 gate and deferred decisions

Slice 1 must create the real targets, presets and dependency manifest, exercise
a clean out-of-tree build, run CTest with no-test failure enabled, verify header
self-containment and provide exact commands in the then-current README.
Do not present the proposed paths above as working setup instructions today.
Compiler/library feature checks and JSON/JCS/SHA known-answer fixtures are
required before they underpin plan identities.

An allocator framework, lock-free queues, coroutine runtime, distributed build,
package-manager migration and stable ABI remain deferred. These can be added
only for a measured need without moving Job, resource or paid-budget authority.

## Primary evidence

Retrieved 2026-09-27; living pages are evidence for interfaces, not a promise of
unchanging releases. The CMake source is explicitly the 3.28 manual.

- T1: [GCC C++ status](https://gcc.gnu.org/projects/cxx-status.html).
- T2: [Clang C++ status](https://clang.llvm.org/cxx_status.html).
- T3: [Microsoft language/library conformance](https://learn.microsoft.com/en-us/cpp/overview/visual-cpp-language-conformance?view=msvc-170).
- T4: [CMake 3.28 presets](https://cmake.org/cmake/help/v3.28/manual/cmake-presets.7.html).
- T5: [Catch2 CMake integration](https://github.com/catchorg/Catch2/blob/devel/docs/cmake-integration.md), including its v3.8.1 example; [upstream releases](https://github.com/catchorg/Catch2/releases).
- T6: [nlohmann SAX interface](https://json.nlohmann.me/features/parsing/sax_interface/) and [3.12.0 release](https://github.com/nlohmann/json/releases/tag/v3.12.0).
- T7: [OpenSSL EVP digest interface](https://docs.openssl.org/3.3/man3/EVP_DigestInit/).
- T8: [Clang ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html).
- T9: [clang-format](https://clang.llvm.org/docs/ClangFormat.html).
- T10: [clang-tidy](https://clang.llvm.org/extra/clang-tidy/).
