# nlohmann JSON

Vendored official single-header release **3.12.0**, under the included MIT license.
The runtime uses its SAX interface behind private compiler implementation files.
No nlohmann type appears in the public API.

Source: https://raw.githubusercontent.com/nlohmann/json/v3.12.0/single_include/nlohmann/json.hpp

SHA-256: `aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`

The compiler enforces its own reviewed bounds during SAX construction; upstream
parse diagnostics are never copied into a runtime error. Canonical export is
RFC 8785 ordering/encoding over owned values and uses OpenSSL EVP SHA-256.
