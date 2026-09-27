# Provisioned upstream sources

Configuration uses these local reviewed files and checks their SHA-256 hashes;
configuration and tests never download dependencies. The exact lock is
[`cmake/dependencies.lock.json`](../cmake/dependencies.lock.json).

- Catch2 **v3.8.1**: unmodified upstream `extras/catch_amalgamated.hpp` and
  `extras/catch_amalgamated.cpp`, Boost Software License 1.0, included license.
  `flamoris_catch2` compiles the real framework and its upstream test main.
- nlohmann/json **v3.12.0**: unmodified upstream `single_include/nlohmann/json.hpp`,
  MIT license; private to compiler translation units.
- OpenSSL libcrypto: explicitly provisioned system dependency, **3.0.13** with
  Ubuntu security package **3.0.13-0ubuntu3.15** in the Linux lane; **3.6.4** on the
  pinned Windows image. Public headers expose no OpenSSL types. Linux downloaded
  package checksums and the tested shared library hash are locked. Windows
  qualification remains pending execution on that image.

Source files can be restored from the versioned upstream locations in the lock,
then checked against the recorded hashes before configuration. No local dependency
cache or machine-specific tool path belongs in source control. Optional OpenCL
headers/loader and the POCL CPU ICD are independently provisioned; offline tests
never probe a device. A POCL result does not qualify any physical GPU.
