# Native fixture qualification

The tests below were executed on 2026-09-27 with GCC 13.3.0, C++20, Debug,
CMake 3.28.3 and Ninja. They qualify only the model, processor, tokenizer and
profile defined in [README.md](README.md). No pretrained model quality,
Qwen/Llama compatibility, universal model support or GPU result is implied.

| Evidence | Executed coverage |
| --- | --- |
| B-REAL01 | Checksum/header/shape checked load, bounded processor/tokenizer, CPU arithmetic known answers, independent binary64 output oracle, full cached/uncached logits |
| B-REAL02 | Every sampled segment compared with the same seed and options after safe-point pause/resume; complete causal state compared |
| B-REAL03 | Injected partial compute failure invalidates resume; stop blocks further useful work; partial initialization and failed physical close retain owned state for acknowledged retry |
| B-REAL04 / B-NATIVE01 / A11 | Independent native workers and Runtime instances; concurrent Runs wait without an execution lease during the first model load, share one immutable model allocation, and release it only after the final reference |
| B-REAL05 | Offload, snapshot, rewind, batching and mid-kernel preemption explicitly unsupported |
| B-REAL06 / B-INPUT01 | Pending token, RNG draws/state, repetition history, literal-grammar position, UTF-8 carry and stop tail preserved; queued injection evaluates pending output first and never draws RNG during prefill; duplicate injection does not append twice |
| B-REAL07 | Japanese and all specified emoji/combining vectors, strict malformed/incomplete UTF-8 and UTF-16 surrogate rejection, byte splitting, processor/tokenizer identity mismatch, special-token collision rejection, and complete scalars at every output budget from 1 through 16 bytes |
| B-OPENCL01 | Real OpenCL matvec and causal attention, CPU parity, complete KV comparisons, cached/uncached inference, identical input token IDs and pins, greedy segment output parity, pause, stop and repeated release |

The separate C08 offline tests inject a scripted worker at the native seam and
exercise the production `InferenceMachine` and `RunController`: current dispatch
checks, pending-slot bounds, cancel before completion, exact deadline, stale
operation generation, continuation ownership and rejected controls. Real worker
integration tests load and run the fixture on the native thread. Scripted tests
are not counted as real native qualification.

The production `RuntimeInstance` native suite executes six cases: pending-token
pause/resume, delayed-token cancellation with withheld host release acknowledgement,
independent instances, a compiled dynamic child followed by exactly one result
injection, concurrent Run model sharing, and actual OpenCL inference through the
host-resource ledger. The dynamic-child test sets `max_workers=1` and compares its
final text with a direct CPU continuation using the same pending token and injected
child result. Test barriers hold real completed worker receipts; they do not replace
model arithmetic. Host authority is a deterministic manual port, so these cases
do not establish the separate external B-HOST01 integration gate.

The actual OpenCL platform was Portable Computing Language (PoCL), driver
`5.0+debian`, LLVM 16.0.6, OpenCL C 1.2. The selected device was
`cpu-haswell-AMD EPYC 7763 64-Core Processor`, vendor `AuthenticAMD`, device type
CPU. This is an actual OpenCL device execution result; no GPU was tested.
The CPU/OpenCL tolerance is `abs(error) <= 2e-5 + 2e-5*abs(cpu)` for FP32 logits,
KV values and operations. The same strict tokenizer IDs are used by both.

The maximum context is 256 byte-token positions (including BOS and all injected
input). Outputs are bounded by 128 tokens, prompts by 255 bytes subject to the
combined context bound, and one prefill segment by 32 tokens (default 8).
Session-private state reserves 131072 bytes; the fixture weights occupy 25984
bytes. OpenCL tests admit at most 65536 bytes of explicit native OpenCL buffers
per operation and check peak usage, zero retained buffers and `clFinish`
quiescence. Driver/compiler/context internals are outside OpenCL's buffer-size
reporting and require an additional conservative registered host envelope;
these buffer tests are not a universal process-RSS bound or cross-process
capacity proof. B-HOST01 remains a separate integration gate.

Native calls are synchronous on a worker thread. Stop/pause takes effect at a
segment boundary, after queue completion; there is no mid-kernel stop claim.
An uncertain queue failure retains buffers and host transfer storage and cannot
produce release success. Partial OpenCL initialization returns its owning compute
object alongside the error; the worker retains it until explicit close succeeds.
Successful close requires acknowledged buffer, kernel, program, queue and context
release. Failed stop/release receipts consume their operation without settling
the lifecycle or resource debt, allowing bounded retry or supervised containment.
A worker/context that cannot quiesce must remain
accounted or be safely contained by the configured supervisor.

To run after provisioning the locked dependencies and a real OpenCL ICD:

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DFLAMORIS_NATIVE_TESTS=ON -DFLAMORIS_ENABLE_OPENCL=ON \
  -DFLAMORIS_OPENCL_TESTS=ON
cmake --build build/native
ctest --test-dir build/native -R 'inference|native' --output-on-failure
```

`FLAMORIS_OPENCL_DEVICE` selects the explicit enumerated device index for the
OpenCL test executable (default 0). No device probing or artifact download
occurs in ordinary offline tests. Selecting the OpenCL lane without a working
device fails the lane; it never reports a skipped or fake test as a pass.

The final targeted GCC Debug execution passed six executables in 0.92 seconds:
native registration, C08 inference, production Runtime native, CPU session, native
worker, and real OpenCL qualification. CPU session qualification contains ten
cases (990 assertions); the Runtime native suite contains six cases.

The CPU qualification was also executed with Clang 18.1.3, warnings-as-errors,
AddressSanitizer and UndefinedBehaviorSanitizer. These four executables passed:
`acceptance_inference_tests_cpp`, `acceptance_runtime_native_tests_cpp`,
`integration_native_native_tests_cpp`, and
`integration_native_inference_worker_tests_cpp` (0.73 seconds for the recorded
run, with five CPU-only Runtime cases). The isolated configure used
`-DFLAMORIS_SANITIZER=address-undefined
-DFLAMORIS_NATIVE_TESTS=ON -DFLAMORIS_ENABLE_OPENCL=OFF
-DFLAMORIS_WARNINGS_AS_ERRORS=ON` with the Clang compiler, followed by those
explicit build targets and:

```sh
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build/native-asan \
  -R '^(acceptance_(inference|runtime_native)_tests_cpp|integration_native_(native|inference_worker)_tests_cpp)$' \
  --output-on-failure
```

LeakSanitizer is unavailable under this execution environment's tracing setup;
leak detection was explicitly disabled. This is an ASan/UBSan result and does not
establish a LeakSanitizer pass. OpenCL was qualified separately on the actual
POCL device above, outside the sanitizer lane.
