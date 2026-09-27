# Native fixture qualification

The tests below were executed on 2026-09-27 with GCC 13.3.0, C++20, Debug,
CMake 3.28.3 and Ninja. They qualify only the model, processor, tokenizer and
profile defined in [README.md](README.md). No pretrained model quality,
Qwen/Llama compatibility, universal model support or GPU result is implied.

| Evidence | Executed coverage |
| --- | --- |
| B-REAL01 | Checksum/header/shape checked load, bounded processor/tokenizer, CPU arithmetic known answers, independent binary64 output oracle, full cached/uncached logits |
| B-REAL02 | Every sampled segment compared with the same seed and options after safe-point pause/resume; complete causal state compared |
| B-REAL03 | Injected partial compute failure invalidates resume; stop blocks further useful work; release follows actual synchronous quiescence |
| B-REAL04 / B-NATIVE01 native holder | Independent native workers and sessions; shared immutable model stays alive until the final explicit session reference release |
| B-REAL05 | Offload, snapshot, rewind, batching and mid-kernel preemption explicitly unsupported |
| B-REAL06 / B-INPUT01 | Pending token, RNG draws/state, repetition history, literal-grammar position, UTF-8 carry and stop tail preserved; queued injection evaluates pending output first and never draws RNG during prefill; duplicate injection does not append twice |
| B-REAL07 | Japanese and all specified emoji/combining vectors, strict malformed/incomplete UTF-8 and UTF-16 surrogate rejection, byte splitting, processor/tokenizer identity mismatch and special-token collision rejection |
| B-OPENCL01 | Real OpenCL matvec and causal attention, CPU parity, complete KV comparisons, cached/uncached inference, identical input token IDs and pins, greedy segment output parity, pause, stop and repeated release |

The separate C08 offline tests inject a scripted worker at the native seam and
exercise the production `InferenceMachine` and `RunController`: current dispatch
checks, pending-slot bounds, cancel before completion, exact deadline, stale
operation generation, continuation ownership and rejected controls. Real worker
integration tests load and run the fixture on the native thread. Scripted tests
are not counted as real native qualification.

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
produce release success. A worker/context that cannot quiesce must remain
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
