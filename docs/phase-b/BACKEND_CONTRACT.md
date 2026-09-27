# Native model execution contract

Phase B design only. No production C++ declarations or executable inference exist.
[Phase A state](../STATE_MACHINES.md), [resources](../RESOURCE_MODEL.md) and
[ADR 0001](../adr/0001-backend-control.md) govern this proposal.

## Boundary

FLAMORIS AI Runtime owns model processing, execution, cache, algorithm state,
bounded steps and cleanup. `NativeModelWorker` owns mutable model/session state;
the Run controller commits Job changes and events. A `ComputeImplementation`
selects FLAMORIS CPU or OpenCL operations inside native execution. It is not a
slot for llama.cpp, vLLM, Transformers, TensorRT-LLM or remote inference.
External AI is a registered Workflow capability with its own effect and outcome
contract. Its output may be bounded and injected at a validated native safe point,
but it never becomes the owner of native state.

## Proposed responsibilities

| Value or operation | Owner and contract |
| --- | --- |
| `ModelDefinition` | Immutable registered artifact, processor/tokenizer, architecture, revision and compatibility identity; no arbitrary path/download from a plan |
| `ExecutionProfile` | Supported model family, controls, numeric precision, limits and compute requirements; initial causal-text profile is narrow and explicit |
| `ComputeImplementation` | CPU reference or OpenCL native operations and allocation receipts; no independent inference loop or Job authority |
| `NativeModelWorker` | Exclusive mutable model/session access, bounded command queue, callback fencing and actual quiescence/release evidence |
| `NativeExecutionState` | Model-neutral envelope: model/config identity, stage, input/output progress, algorithm-state reference, resource IDs, validity and suspension generation |
| `CausalTextState` | Profile-owned token position, KV/cache, pending token, sampler/RNG/penalty/grammar, UTF-8 carry and stop matcher |
| `StateReference` | Job-owned resumable handle to worker state; nonportable and process-lifetime; retained memory remains charged |

The initial worker may serialize native calls and use one sequence per context.
These are first-profile qualification limits, not universal Runtime invariants.
Multiple Runtime instances in one process are not permanently prohibited.
Any process-scoped OpenCL context/cache has its own explicit lifetime and cannot
be destroyed while a user remains.

## Segment protocol

1. Admission pins model/profile/processor and validates finite context, output,
   steps, resources and current policy. The Resource Manager reserves a complete
   conservative vector before a native allocation or useful segment.
2. An admitted Job triggers model load. Native weights/context/state allocations
   are recorded uniquely; shared residency has a separate ledger owner and is
   never released merely because the initiating Job ends.
3. Tokenize/process, prefill and decode operate in bounded segments under a
   current dispatch permit. A worker returns an owned receipt; it does not
   autonomously continue generation or publish token events from a callback.
4. The controller accepts a consistent state version and observation together,
   then applies cancel/pause/yield/child dispatch before the next segment.
   Stop request, quiescence and physical release are distinct receipts.
5. Preserve state only at a proven safe point. Resume validates exact
   model/config/profile/state and resource generations with a fresh permit.
   Re-inference from a prompt after state loss is new work, never equivalent
   resume. Cleanup retains uncertain allocations until actual evidence.

For causal text, evaluated input position and emitted output position differ:
a sampled token may be pending evaluation on the next step. Pause or bounded
input injection preserves that token exactly once together with sampler/RNG,
grammar, stop matcher and partial text-decoder state. A cache copy by itself
is not a complete continuation. Profile-specific correctness tests compare
uninterrupted against paused execution and cached against uncached reference.
Other model families define their own state and safe points without inheriting
causal-text fields.

## Native qualification and compatibility

Start with a deterministic fake for control ordering, then native CPU as the
correctness oracle. OpenCL is the first accelerated compute direction: record
device, driver, model/artifact, precision, allocation envelope, tolerance and
CPU/OpenCL parity evidence. Numerical equivalence is tested at the relevant
operation/output level; bitwise identity across devices is not assumed.
Unsupported pause, offload, snapshot, rewind or batching is reported as
unsupported. No real capability is inferred from a fake test.

Model and state compatibility uses FLAMORIS-owned revisioned identities, not
third-party backend versions. A changed model, tokenizer/processor, profile,
numeric representation or incompatible compute state invalidates resume unless
an explicit migration is implemented and qualified. No portable live-state
serialization is promised in Phase C.

The existing private `flamoris-LLM` is a conceptual and testing foundation:
tokenizer/model separation, incremental cached execution, CPU reference,
OpenCL compute and parity methodology. No private source is copied without
explicit publication, licensing and provenance review.
