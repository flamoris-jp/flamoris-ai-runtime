# Tiny causal FP32 fixture v1

This self-authored, untrained numerical conformance fixture is licensed under the
repository's Apache License 2.0. No external model weights, datasets, private
source or pretrained architecture implementation are incorporated. It establishes
native arithmetic, incremental state and control behavior only; it is not a
pretrained language model and makes no language-quality or model-family claim.

The registered model is `flamoris.tiny-causal.v1`, execution profile
`flamoris.tiny-causal-fp32.v1`. Its SHA-256 is
`5cd5a0bacb4cce1efd801fe2a4d7c1347467538ef6e393b37d955d2499ef74e2`.
The file starts with `FTRTINY1`, followed by little-endian unsigned 32-bit width,
vocabulary and context dimensions (8, 258, 256), then little-endian IEEE FP32
embedding, position embedding, query/key/value, attention output, feed-forward
and vocabulary projection matrices. Matrices are row-major. The single causal
attention layer attends only to positions already evaluated; a residual tanh
feed-forward transform follows its residual output. There is no layer norm,
RoPE, quantization, dropout or implied Qwen/Llama compatibility.

`generate_fixture.py` is the complete deterministic generation provenance.
It uses an explicitly specified integer LCG and power-of-two rational weights,
so regeneration does not depend on Python random or platform floating rounding.
Reproduce with `python3 fixtures/native/generate_fixture.py`; verify with
`sha256sum -c fixtures/native/tiny-causal-v1.sha256` from this directory (or pass
its file to the loader with the registered checksum). The loader rejects size,
header, checksum, shape and non-finite-value mismatches before publishing a model.

The tokenizer `flamoris.byte-utf8.v1` maps each input UTF-8 byte directly to an ID
0–255, applies no normalization and adds no hidden special tokens. BOS 256 is
inserted by the registered processor; EOS 257 is handled explicitly. Literal
strings resembling control tokens remain ordinary bytes. The independently pinned
`flamoris.raw-prompt.v1` processor performs no template expansion. Different
processor or tokenizer pins require a new plan and reject resume. CPU and OpenCL
use exactly these same processor/tokenizer identities and token IDs.

The independently written C++ implementation follows the reviewed private
foundation's general separation of tokenizer, model, cache and compute plus
CPU-reference/parity methodology. Current private source was inspected before
implementation; no private source, tests, paths or implementation details were
copied or published. Comparative runtime evidence remains in ADR 0001.
