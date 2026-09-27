#!/usr/bin/env python3
"""Apache-2.0. Generate deterministic, untrained conformance weights; no third-party data."""
import hashlib
import pathlib
import struct

root = pathlib.Path(__file__).parent
width, vocab, context = 8, 258, 256
count = vocab * width + context * width + 5 * width * width + vocab * width
state = 0x464c414d
weights = []
for _ in range(count):
    state = (1664525 * state + 1013904223) & 0xffffffff
    weights.append(((state >> 16) - 32768) / 131072.0)
raw = b"FTRTINY1" + struct.pack("<III", width, vocab, context) + struct.pack(f"<{count}f", *weights)
(root / "tiny-causal-v1.bin").write_bytes(raw)
(root / "tiny-causal-v1.sha256").write_text(hashlib.sha256(raw).hexdigest() + "  tiny-causal-v1.bin\n")
