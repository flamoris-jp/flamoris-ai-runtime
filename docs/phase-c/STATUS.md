# Phase C implementation evidence

Source: reviewed main `367ead16596d0b737bbc6c362e89ed397d93a56b` and
[Issue #9](https://github.com/flamoris-jp/flamoris-ai-runtime/issues/9).

The Phase C baseline implementation is under final integration review. The local
GCC and Clang sanitizer lanes below are executable evidence for this revision;
they are not a GitHub CI result or a deployed-host qualification. Phase A semantics
and reviewed Phase B implementation contracts remain authoritative. A design
scenario, a test label, and a passing qualification lane are different evidence.

The delivery scope includes C01–C12 and C10a. Slice boundaries are implementation,
commit, test and internal review boundaries. There are no intermediate PRs;
one final PR is reserved for complete baseline review. No automatic merge or
deployment is authorized by this work.

## Qualification boundaries

- Offline deterministic kernel acceptance must exercise production components.
- Native CPU qualification uses a pinned, licensed model/processor/tokenizer
  fixture and checks cached/uncached and pause/resume equivalence.
- Native OpenCL qualification requires an actual device and driver, declared
  numeric tolerances, matching input identities and lifetime/accounting evidence.
- A CPU OpenCL device, if qualified, does not establish GPU qualification.
- External host enforcement and on-demand activation require separate real host
  integration evidence; deterministic protocol fakes do not certify a live service.
- Windows/compiler/static/sanitizer lanes are reported only when actually run.
- Optional strict monetary guarantees, durable recovery, universal model-family
  support, offload/snapshot/rewind and arbitrary external endpoints are excluded.

## Local verification on 2026-09-27

These commands were run on the working branch after activation integration
(`e193808`). The build directories are scratch artifacts. The OpenCL ICD was
PoCL 5.0 on an actual CPU OpenCL device; the GPU on LIME was not exercised.

| Lane | Actual result |
| --- | --- |
| Fresh GCC 13.3.0 C++20, warnings as errors, native CPU tests | Configure and build passed; 26/26 CTest cases passed. OpenCL disabled in this build. |
| Incremental GCC 13.3.0 with OpenCL and PoCL CPU device | Build passed; 27/27 CTest cases passed, including real OpenCL parity. |
| Clang 18.1.3 ASan/UBSan, `ASAN_OPTIONS=detect_leaks=0` | Build passed; 23/23 offline CTest cases passed. Native fixture and OpenCL tests were disabled in this configuration. LeakSanitizer was not run. |
| Clang 18.1.3 TSan, native CPU enabled | Build passed; 26/26 CTest cases passed. OpenCL disabled in this configuration. Four first-pass scratch executables were relinked after permission errors before the complete rerun. |
| clang-format 18.1.3 | All project headers and source/test translation units pass `--dry-run --Werror` after formatting fixes. |
| clang-tidy 18.1.3, three Runtime composition translation units | Completed with nonfatal performance and intentional process-lifetime ownership warnings; this was a targeted local check, not the full CI translation-unit lane. |
| Documentation and source-boundary scripts | `check_links.py`, `check_docs.py`, `check_boundaries.py` passed. |

The fresh GCC configuration used:

```sh
cmake -S . -B ../build-resume -G 'Unix Makefiles' -DCMAKE_CXX_COMPILER=/usr/bin/c++ \
  -DBUILD_TESTING=ON -DFLAMORIS_NATIVE_TESTS=ON -DFLAMORIS_WARNINGS_AS_ERRORS=ON
cmake --build ../build-resume --parallel 4
ctest --test-dir ../build-resume --output-on-failure --parallel 4
```

The OpenCL build used the existing `../build-native` CMake configuration with
`FLAMORIS_ENABLE_OPENCL=ON`, `FLAMORIS_OPENCL_TESTS=ON`, and
`FLAMORIS_NATIVE_TESTS=ON`. It was rebuilt and tested with:

```sh
cmake --build ../build-native --parallel 4
ctest --test-dir ../build-native --output-on-failure --parallel 4
```

The OpenCL commands additionally set `LD_LIBRARY_PATH` to the locally extracted
package libraries, `OCL_ICD_VENDORS` to the locally extracted PoCL ICD directory,
and `POCL_RESOURCES_DIR` to its resources. Clang sanitizer build and test used:

```sh
cmake --build build/clang-asan --parallel 4
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/clang-asan \
  --output-on-failure --parallel 2
```

The sanitizer directory had `FLAMORIS_SANITIZER=address-undefined` and native
tests disabled. Exact package identities and the qualified tiny-model profile
are in the dependency lock and [native qualification](../../fixtures/native/QUALIFICATION.md).
Two first-pass test executables had non-executable file modes in the local scratch
build; after relinking them, the entire 23-test lane passed. A first-pass GCC
scratch executable was similarly relinked before the 26-test rerun. No passing
result is claimed for those incomplete first attempts.

The TSan configuration had `FLAMORIS_SANITIZER=thread`, native CPU tests enabled,
and OpenCL disabled. The final commands were:

```sh
cmake --build build/root-tsan-review --parallel 2
ctest --test-dir build/root-tsan-review --output-on-failure --parallel 2
```

Remaining release evidence: final cross-layer review, full CI lanes on the final
PR, physical LIME GPU qualification and deployed host
enforcement. The latter two are separate integration gates and are not inferred
from the PoCL or deterministic-host-port tests. Windows is not claimed as passing
here.


## Generation composition bridge: compiler slice

[Media-to-Runtime lowering](MEDIA_LOWERING.md) implements pure exact-pin operation
lowering through the existing compiler. The typed boundary retains media and Runtime
identities, complete effects/limits and occurrence mapping. Offline rejection tests
cover compiler portions of B01/B02/B03/B04/B06/B07. This does not qualify an executable
cross-service bridge, real provider/host or shared Studio delegation; #19 stays open.
