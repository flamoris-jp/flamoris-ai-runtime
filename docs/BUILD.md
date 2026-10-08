# Build and validation

The implementation uses C++20, CMake 3.28 or later and Ninja. The reviewed
compiler families are GCC 13, Clang 18 and Visual Studio 2022. Exact dependency,
compiler and runner-image identities are recorded in
[`dependencies.lock.json`](../cmake/dependencies.lock.json). Catch2 and the JSON
parser are vendored and checked at configure time. OpenSSL Crypto must already
be provisioned; configuration never downloads a dependency or model.

## Linux

Provision GCC 13.3.0 or Clang 18.1.3, CMake, Ninja and the locked OpenSSL package
before configuring. The optional `FLAMORIS_LOCKED_TOOLCHAIN` gate rejects a
different compiler patch and, on Linux, a libcrypto binary outside the exact
reviewed hash allowlist. Ubuntu package revisions `.15` and `.16` are recorded
separately; CI pairs each runner image with its reviewed package revision. If the
dependencies live outside the standard search path, pass `OPENSSL_ROOT_DIR` and
put the selected CMake/Ninja/compiler executables on `PATH`.

```sh
cmake --preset gcc-debug -DFLAMORIS_LOCKED_TOOLCHAIN=ON
cmake --build --preset gcc-debug --parallel 2
ctest --preset gcc-debug --no-tests=error
```

The `gcc-release` and `clang-debug` presets run the same offline suite in their
respective configurations. Public headers compile separately so an accidental
include order cannot satisfy a missing dependency. Production libraries and
test support remain separate targets.

```sh
cmake --preset clang-asan
cmake --build --preset clang-asan --parallel 2
ctest --preset clang-asan --no-tests=error

cmake --preset clang-tsan
cmake --build --preset clang-tsan --parallel 2
ctest --preset clang-tsan --no-tests=error
```

The first sanitizer preset enables AddressSanitizer and UndefinedBehaviorSanitizer;
the second enables ThreadSanitizer. A sandbox or tracing environment can prevent
a sanitizer runtime from starting. Record that as an unavailable lane, not a
passing test. In this implementation session LeakSanitizer was unavailable;
the recorded ASan/UBSan native runs explicitly set `ASAN_OPTIONS=detect_leaks=0`.

## Native qualification

The small licensed native fixture is checked into the repository. It needs no
account, network access or model download. Enable the additional real native
worker and numerical qualification suites explicitly:

```sh
cmake --preset gcc-debug -DFLAMORIS_NATIVE_TESTS=ON
cmake --build --preset gcc-debug --parallel 2
ctest --preset gcc-debug --no-tests=error
```

OpenCL additionally requires provisioned headers, an ICD loader and a working
device. It is opt-in and is never silently substituted with CPU arithmetic:

```sh
cmake --preset gcc-debug -DFLAMORIS_NATIVE_TESTS=ON \
  -DFLAMORIS_ENABLE_OPENCL=ON -DFLAMORIS_OPENCL_TESTS=ON
cmake --build --preset gcc-debug --parallel 2
ctest --preset gcc-debug --no-tests=error
```

`FLAMORIS_OPENCL_DEVICE` selects the test device index (default `0`). The
[qualification record](../fixtures/native/QUALIFICATION.md) states the actual
device, driver, tolerance, supported profile and ownership limits. POCL CPU
qualification does not establish physical GPU or deployed host conformance.

## Windows

Use a Visual Studio 2022 x64 developer shell with Ninja, CMake and the locked
OpenSSL 3.6.4 or 3.6.5 installation available. Other Windows OpenSSL patches
are rejected until reviewed. `msvc-debug` and `msvc-release` are the CI
presets. Their existence is not evidence of a successful Windows run; consult
the [implementation evidence](phase-c/STATUS.md) and the actual CI result.

```bat
cmake --preset msvc-debug -DFLAMORIS_NATIVE_TESTS=ON
cmake --build --preset msvc-debug --parallel 2
ctest --preset msvc-debug --no-tests=error
```

## Source and documentation checks

```sh
python3 cmake/check_links.py
python3 tools/check_docs.py
python3 tools/check_boundaries.py
rg --files include src tests -g '*.hpp' -g '*.cpp' | xargs clang-format-18 --dry-run --Werror
```

The boundary checker catches forbidden implementation dependencies; it is not a
substitute for lifecycle or authority tests. The CI workflow also builds project
translation units with clang-tidy. Vendored translation units are excluded from
project style analysis while their contents remain checksum verified.

External host grants are supplied only through a trusted configured host port.
The default host authority is unavailable. Running an offline protocol fixture
does not register or authorize a live host service.
