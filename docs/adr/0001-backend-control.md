# ADR 0001: FLAMORIS native model runtime and compute

## Status

Proposed for Phase B review, 2026-09-27 JST (sources retrieved 2026-09-26 UTC).
This is source research and an implementation decision, not an integration test
report. No model was loaded, benchmark run, or backend implemented in this task.
Phase A [execution](../EXECUTION_MODEL.md), [state](../STATE_MACHINES.md), and
[resource](../RESOURCE_MODEL.md) semantics remain authoritative.

## Decision

FLAMORIS AI Runtime **is** the native model runtime. It owns model loading,
processing/tokenization, execution state/cache, incremental inference, sampling,
control points and cleanup. The initial causal-text profile is an implementation
slice, not the universal Runtime state. Begin with a deterministic fake, then an
independently implemented native CPU correctness reference, then native OpenCL
compute and CPU/OpenCL parity tests. CPU and OpenCL are compute implementations
within this Runtime, not interchangeable third-party model runtimes.

The four third-party projects below are comparative research evidence only. No
llama.cpp embedding, provider-specific inference-backend API, process-global
llama lifecycle or adapter compatibility is a Phase C requirement. External AI
runtimes/providers, if used, are registered Workflow external capabilities.
They cannot own FLAMORIS Run/Job/Continuation/native inference state.

## Reproducible evidence

Default-branch heads were resolved through the connected GitHub integration,
then files were read at these exact commits. They are research pins, not an
instruction to track a moving branch or a claim of release stability. Phase C
locks a reviewed dependency revision and repeats qualification on upgrades.

| Runtime | Inspected revision | Head commit date (UTC) |
| --- | --- | --- |
| llama.cpp | `2145525a4081d66ff1a87cf43ef809f95a85ac0c` | 2026-09-26 |
| Transformers | `96331a9f93b72697f160a958d2883d4b49a56739` | 2026-09-26 |
| vLLM | `379e9a1ea8a5995464d9bf775bcd36bb03a0995f` | 2026-09-26 |
| TensorRT-LLM | `b88149e535265b52e8ddb1339e35710e10b7ecf8` | 2026-09-26 |

Primary source keys used below:

- **L1**: [llama.h](https://github.com/ggml-org/llama.cpp/blob/2145525a4081d66ff1a87cf43ef809f95a85ac0c/include/llama.h), notably `llama_decode`, `llama_synchronize`, memory/state APIs, sampler ownership, CPU abort callback and global logging comments.
- **L2**: [simple embedded example](https://github.com/ggml-org/llama.cpp/blob/2145525a4081d66ff1a87cf43ef809f95a85ac0c/examples/simple/simple.cpp), showing separate model, context, tokenization and sampling progression.
- **H1**: [cache explanation](https://github.com/huggingface/transformers/blob/96331a9f93b72697f160a958d2883d4b49a56739/docs/source/en/cache_explanation.md) and [cache strategies](https://github.com/huggingface/transformers/blob/96331a9f93b72697f160a958d2883d4b49a56739/docs/source/en/kv_cache.md).
- **H2**: [logits processors](https://github.com/huggingface/transformers/blob/96331a9f93b72697f160a958d2883d4b49a56739/src/transformers/generation/logits_process.py), [stopping criteria](https://github.com/huggingface/transformers/blob/96331a9f93b72697f160a958d2883d4b49a56739/src/transformers/generation/stopping_criteria.py) and [streamers](https://github.com/huggingface/transformers/blob/96331a9f93b72697f160a958d2883d4b49a56739/src/transformers/generation/streamers.py).
- **H3**: [continuous batching API](https://github.com/huggingface/transformers/blob/96331a9f93b72697f160a958d2883d4b49a56739/docs/source/en/continuous_batching.md) and [its architecture](https://github.com/huggingface/transformers/blob/96331a9f93b72697f160a958d2883d4b49a56739/docs/source/en/continuous_batching_architecture.md).
- **V1**: [AsyncLLM](https://github.com/vllm-project/vllm/blob/379e9a1ea8a5995464d9bf775bcd36bb03a0995f/vllm/v1/engine/async_llm.py) and [engine core](https://github.com/vllm-project/vllm/blob/379e9a1ea8a5995464d9bf775bcd36bb03a0995f/vllm/v1/engine/core.py), especially `abort`, `pause_generation`, `pause_scheduler`, and `resume_generation`.
- **V2**: [sleep mode](https://github.com/vllm-project/vllm/blob/379e9a1ea8a5995464d9bf775bcd36bb03a0995f/docs/features/sleep_mode.md), [KV offloading](https://github.com/vllm-project/vllm/blob/379e9a1ea8a5995464d9bf775bcd36bb03a0995f/docs/features/kv_offloading_usage.md), [custom logits processors](https://github.com/vllm-project/vllm/blob/379e9a1ea8a5995464d9bf775bcd36bb03a0995f/docs/features/custom_logitsprocs.md) and [metrics](https://github.com/vllm-project/vllm/blob/379e9a1ea8a5995464d9bf775bcd36bb03a0995f/docs/design/metrics.md).
- **T1**: [current overview](https://github.com/NVIDIA/TensorRT-LLM/blob/b88149e535265b52e8ddb1339e35710e10b7ecf8/README.md), [LLM API source](https://github.com/NVIDIA/TensorRT-LLM/blob/b88149e535265b52e8ddb1339e35710e10b7ecf8/tensorrt_llm/llmapi/llm.py), and [Python executor](https://github.com/NVIDIA/TensorRT-LLM/blob/b88149e535265b52e8ddb1339e35710e10b7ecf8/tensorrt_llm/_torch/pyexecutor/py_executor.py).
- **T2**: [KV cache system](https://github.com/NVIDIA/TensorRT-LLM/blob/b88149e535265b52e8ddb1339e35710e10b7ecf8/docs/source/features/kvcache.md) and [current C++ request/config/KV-event types](https://github.com/NVIDIA/TensorRT-LLM/blob/b88149e535265b52e8ddb1339e35710e10b7ecf8/cpp/include/tensorrt_llm/executor/executor.h).
- **T3 (legacy evidence only)**: [legacy Executor API](https://github.com/NVIDIA/TensorRT-LLM/blob/b88149e535265b52e8ddb1339e35710e10b7ecf8/docs/source/legacy/advanced/executor.md). Its historical `enqueueRequest`, `cancelRequest`, statistics and C++ logits callback design is useful precedent; it is **not** evidence that this Executor class exists in the inspected current header. Current `LLM` selects the PyTorch backend.

## Control-point matrix

“Possible” below is an inference about writing a version-specific adapter, not a
FLAMORIS capability declaration. Public server streaming alone proves neither
state preservation nor control over the next model iteration.

| Control point | llama.cpp embedded (L1–L2) | Transformers (H1–H3) | vLLM (V1–V2) | TensorRT-LLM current (T1–T2) |
| --- | --- | --- | --- | --- |
| Tokenization | Vocabulary and explicit token/piece APIs | Tokenizer separate from model; custom loops illustrated | Frontend/rendering and token-input processing precede engine submission | LLM preprocessing/tokenizer; executor requests carry tokens |
| Prefill / decode | Caller supplies token batches/positions; bounded chunks then single-token calls | Direct model forward plus cache can be caller-driven; `generate` and batching manager own loops | Engine scheduler/model executor own iterations | Python executor/model engine own iterations and context/generation stages |
| Logits / sampling | Caller can obtain logits and apply/accept/clone supported sampler state | `LogitsProcessor` / stopping hooks; direct forward permits own sampler | Batched custom processors, runner-specific interface | Sampling parameters/processors in Python pipeline; historical C++ callbacks are separate legacy evidence |
| KV lifetime | Context owns native state; caller controls context lifetime | Explicit cache object/tensors; cache strategy varies by model | Engine block allocator/prefix cache and request scheduler own it | Executor cache managers own pools, blocks, retention and sharing |
| Streaming | Caller publishes after chosen state commit | Streamer or batching partial results | Async request outputs | Async/streaming request results |
| Cancellation | Between calls; CPU-only abort callback; partial ubatches may remain after abort/fatal error | Stopping criteria are loop checks; batching manager has cancellation | `abort` requests engine cancellation | `cancel_request` enqueues cancellation; not a synchronous device-stop proof |
| Preserve pause | Feasible by withholding next call while context/sampler stay alive after synchronization | Feasible in custom loop if all cache/RNG/decoder state retained; no universal checkpoint from stopping criteria | Current engine-wide `keep` pause exists; use `clear_cache=False` for preservation. Not a per-Job detached state handle | Scheduler pause/recompute paths exist internally; inspected public surface does not establish arbitrary per-Job preserved pause |
| Snapshot | Context/sequence data APIs; not a complete FLAMORIS session snapshot | Cache object alone omits generation/RNG/decoder state; not portable across all models | No complete FLAMORIS Job snapshot established by examined API | Cache transfer/retention is not a complete generation-session snapshot |
| Offload | State APIs can support future copy/restore; serialized bytes do not prove source memory release | Offloaded caches; batching CPU swap with soft-reset fallback when unavailable | Prefix offload connectors exist; sleep level 1 discards KV and level 2 also weights | Host cache tier and prioritized eviction/reuse; separate transfer completion/accounting needed |
| Rewind | Sequence range removal may reject partial removal; sampler/history must rewind too | Cache crop depends on cache type; generation state remains caller obligation | No general user-controlled exact rewind established | No general user-controlled exact rewind established |
| Continuous batching | Batch sequence IDs enable it; library does not provide our Job scheduler | Current manager has scheduling, cache admission and chunked prefill | Core serving feature owned by its scheduler | In-flight scheduling owned by its executor |
| Memory/resource control | Explicit model/context lifetimes, but internal buffers need conservative bounds | PyTorch/cache allocator ownership; references and allocator pools differ | Pool admission/configuration; memory gauges are observations | Pool/configuration controls; shared cached blocks outlive requests |
| Observability | FLAMORIS can instrument each call; header advises own timing over example perf helpers | Caller hooks/outputs and batching status | Engine/request metrics and traces | Iteration/request statistics and cache events |
| Embedded vs server | Native C API permits direct loop ownership; HTTP loses these control points | Python embedded/custom loop possible; additional language/runtime boundary for C++ | Python engine/server gives rich serving control but retains its scheduler | Modern PyTorch/Python execution surface; older C++ Executor documentation must not be conflated |

## Consequences for FLAMORIS

1. Keep a model-neutral native execution envelope: model/configuration identity,
   execution stage, state references, progress, algorithm-state identity,
   resource allocations and suspension validity. Causal-text KV, pending token,
   sampler/RNG, penalty/grammar, decoder carry and stop matcher belong to its
   causal-text profile. Do not promise these fields for Vision, embedding or audio.
2. Own bounded native steps and require distinct quiescence and release evidence.
   An abort request or freed logical reference is not physical release.
3. Use CPU as the correctness oracle, including cached versus uncached decoding,
   then compare native OpenCL results under documented tolerances. Numerical
   parity is a qualification task, not a claim of current implementation.
4. Keep Scheduler ownership of Jobs. A future batching optimization is internal
   to FLAMORIS and needs explicit permits; one sequence/context and serialized
   calls are initial qualification limits, not permanent architecture rules.
5. Third-party runtime APIs supply design comparisons, not substitute inference
   state/lifetime contracts. Their licenses matter only if a separate Workflow
   integration actually incorporates them.

## Alternatives considered

Embedding llama.cpp, wrapping Transformers, vLLM or TensorRT-LLM, and generic
server completion APIs would delegate some execution control to their own
interfaces. They may be useful through Workflow capability registration, but
are outside native inference. The earlier embedded-llama.cpp selection and its
process-global construction guard are superseded by this decision.

## Private foundation reuse and publication boundary

The current private `flamoris-LLM` foundation was inspected through authenticated
GitHub access on 2026-09-27 JST. Revision/file-level evidence remains outside the
public repository; private source, paths, implementation internals and topology
are not published here.

Reuse independently implemented concepts: separate tokenizer/model/compute
contracts, bounded prefill versus incremental decode, complete request-owned
resume state, correctness-first CPU reference methodology, cached/uncached and
backend parity tests, opt-in acceleration, and strict artifact compatibility.
Candidate future migrations are limited to independently separable numerical
reference/test utilities after a specific provenance review.

Keep model-specific internals and deployment/configuration private/separate.
Do not inherit a convenience generation API as the control contract, global
request state, assumed reentrancy, or a compute abstraction as evidence of
scheduler/resource/interrupt integration. Missing evidence includes native
integration conformance, controlled lifetime/threading guarantees and a cleared
source-publication license. **No private code or tests are transplanted in
Phase B or assumed reusable in Phase C.** Any future source migration requires
explicit authorization plus provenance and license clearance.

## Dependency and licensing boundary

No third-party model runtime dependency is selected. Model weights, tokenizers,
datasets and fixtures require their own provenance, license and checksum checks.
OpenCL is the approved native compute API direction. Private `flamoris-LLM`
source is not copied into this public repository without a specific publication,
provenance and licensing review. The linked third-party source revisions above
remain research pins, not dependency pins.

## Qualification gate and remaining evidence

Phase C must prove native CPU load/tokenization, bounded prefill/decode,
stateful cache/sampler progress, pause/resume equivalence at safe points, cancel,
resource ownership and release. OpenCL then needs a qualified device/profile,
measured bounds and CPU/OpenCL parity. Unsupported controls reject explicitly.
No model was loaded or tested in this Phase B design correction.
