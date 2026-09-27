#!/usr/bin/env python3
"""Apache-2.0 independent full-sequence binary64 numerical oracle for the tiny fixture.

This intentionally does not share the C++ cache/evaluation implementation. It
builds every input projection first, then evaluates only the final causal row.
No external numerical or model-runtime dependency is required.
"""
import json
import math
import pathlib
import struct

root = pathlib.Path(__file__).parent
raw = (root / "tiny-causal-v1.bin").read_bytes()
width, vocab, context = struct.unpack_from("<III", raw, 8)
weights = list(struct.unpack_from(f"<{(len(raw)-20)//4}f", raw, 20))
ids = [256, 72, 105, 230, 151, 165]
offset = 0

def take(rows):
    global offset
    matrix = [weights[offset+r*width:offset+(r+1)*width] for r in range(rows)]
    offset += rows * width
    return matrix

embedding, positional = take(vocab), take(context)
wq, wk, wv, wo, wf, head = (take(rows) for rows in [width]*5 + [vocab])

def linear(matrix, vector):
    return [sum(a*b for a,b in zip(row, vector)) for row in matrix]

hidden = [[a+b for a,b in zip(embedding[token], positional[p])] for p,token in enumerate(ids)]
queries = [linear(wq,h) for h in hidden]
keys = [linear(wk,h) for h in hidden]
values = [linear(wv,h) for h in hidden]
scores = [sum(a*b for a,b in zip(queries[-1],k)) / math.sqrt(width) for k in keys]
maximum = max(scores)
probabilities = [math.exp(s-maximum) for s in scores]
total = sum(probabilities)
probabilities = [p/total for p in probabilities]
attended = [sum(p*v[d] for p,v in zip(probabilities,values)) for d in range(width)]
residual = [a+b for a,b in zip(hidden[-1],linear(wo,attended))]
final = [a+math.tanh(b) for a,b in zip(residual,linear(wf,residual))]
logits = linear(head,final)
fixture = {"input_ids": ids, "logits_first_8": logits[:8], "argmax": max(range(vocab),key=lambda i:logits[i]),
           "comparison_atol": 2e-7, "comparison_rtol": 1e-5}
(root / "reference-v1.json").write_text(json.dumps(fixture,indent=2)+"\n")
print(json.dumps(fixture))
