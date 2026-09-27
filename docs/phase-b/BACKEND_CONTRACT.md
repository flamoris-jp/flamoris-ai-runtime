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
| `ModelDefinition` | Immutable registered weights/artifact, architecture, revision and compatible processor/tokenizer identity; no arbitrary path/download from a plan |
| `ProcessorDefinition` | Immutable registered chat/prompt template revision, modality input preprocessing, output postprocessing, model binding and compatibility fingerprint; references a `TokenizerDefinition` when text tokenization is required. A template change updates the processor identity without changing the tokenizer identity |
| `TokenizerDefinition` | Immutable registered vocabulary/merge or token mapping and model binding, normalization and pre-tokenization rules, special token IDs and policy, BOS/EOS behavior, encode/decode semantics and compatibility fingerprint; the same tokenizer may be referenced by different processor templates |
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

1. Admission pins model/profile/processor/tokenizer identities and validates finite context, output,
   steps, resources and current policy. The Resource Manager reserves a complete
   conservative vector before a native allocation or useful segment.
2. An admitted Job triggers model load. Native weights/context/state allocations
   are recorded uniquely; shared residency has a separate ledger owner and is
   never released merely because the initiating Job ends.
3. Registered processing/tokenization, prefill and decode operate in bounded segments under a
   current dispatch permit. A worker returns an owned receipt; it does not
   autonomously continue generation or publish token events from a callback.
4. The controller accepts a consistent state version and observation together,
   then applies cancel/pause/yield/child dispatch before the next segment.
   Stop request, quiescence and physical release are distinct receipts.
5. Preserve state only at a proven safe point. Resume validates exact
   model/config/profile/processor/tokenizer/state and resource generations with a fresh permit.
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
third-party backend versions. A changed model, processor/tokenizer fingerprint,
profile, numeric representation or incompatible compute state invalidates both
plan pins and resume unless an explicit migration is implemented and qualified.
CPU and OpenCL consume token IDs produced by the same pinned tokenizer; compute
choice never changes tokenization semantics. A prompt-template-only revision
changes the processor pin, not tokenizer identity or the KV representation; an
existing plan/Continuation still rejects a mismatched processor pin rather than
silently resuming with different prompt semantics. No portable live-state
serialization is promised in Phase C.

The first causal-text qualification includes known token vectors and
encode/decode round trips against the registered normalization policy; exact
byte round trips are required only where normalization is disabled. Validate strict UTF-8
input and output, including Japanese, multi-byte emoji, ZWJ sequences, variation
selectors, skin-tone modifiers, regional-indicator flags, and combining marks.
Normalization is applied only when the registered tokenizer explicitly requires
it; decomposed and precomposed forms must follow that pinned rule. A token may
end inside a UTF-8 sequence: the profile-owned incremental decoder retains
incomplete bytes and emits only valid completed scalars, never a replacement
character as a shortcut. A visible emoji sequence may span multiple tokens or
Unicode scalars; no one-emoji/one-token or one-grapheme/one-token promise exists.
Reject malformed UTF-8, unpaired surrogate input at conversion boundaries,
invalid special-token collisions and incompatible tokenizer revisions before
execution/resume. Special-token handling must not reinterpret ordinary Unicode
as control tokens. Preserve decoder carry across pause/resume and test split
sequences and final incomplete bytes explicitly. These are conformance
requirements for an implemented native tokenizer, not claims of current support.

The existing private `flamoris-LLM` is a conceptual and testing foundation:
tokenizer/model separation, incremental cached execution, CPU reference,
OpenCL compute and parity methodology. No private source is copied without
explicit publication, licensing and provenance review.
