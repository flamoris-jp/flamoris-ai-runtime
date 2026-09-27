# Bounded command-line example

Build the `flamoris-runtime` target using the repository's documented CMake
toolchain, then validate the offline example:

```sh
build/gcc-debug/flamoris-runtime validate examples/minimal-submit.json
```

The command runs the production bounded parser and compiler against the explicitly
registered pure `algorithm.identity` contract and prints the semantic plan
fingerprint. It does not admit or dispatch a Run. The capability returns the exact
bounded object it receives: `{"text":"Hello, FLAMORIS"}` in this example.

`flamoris-runtime serve` exposes the production `flamoris.control/1` JSON-lines
facade with one process-local CLI owner. Its standalone configuration uses the
unavailable host authority. Submissions cannot acquire execution capacity and may
time out; cancel/status/result/event requests use the same Runtime controllers.
Useful execution requires a trusted embedding with registered resource envelopes
and matching host receipts. Portable workflow JSON cannot supply that authority.

The CLI accepts at most 1 MiB per submission file or control line. It has no
arbitrary endpoint, credential, model-download or shell-execution option. Native
CPU and OpenCL qualification use the separately documented pinned fixture lanes.
