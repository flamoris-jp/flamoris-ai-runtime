#!/usr/bin/env python3
"""Static dependency guardrails. This does not certify behavioral acceptance."""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
failures = []
for path in sorted((ROOT / "src").rglob("*")):
    if path.suffix not in {".cpp", ".hpp", ".h", ".cl"}:
        continue
    text = path.read_text(encoding="utf-8")
    relative = path.relative_to(ROOT)
    includes = re.findall(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', text, re.M)
    for include in includes:
        if any(name in include.lower() for name in ("llama.h", "transformers/", "vllm/", "tensorrt")):
            failures.append(f"{relative}: third-party native model runtime include {include}")
        if relative.parts[1] in {"domain", "kernel"}:
            if any(name in include.lower() for name in ("nlohmann", "openssl/", "cl/cl", "curl/", "catch")):
                failures.append(f"{relative}: external library crossed kernel/domain boundary: {include}")
        if relative.parts[1] == "kernel" and include in {
            "flamoris/runtime/native.hpp", "flamoris/runtime/compute.hpp",
            "flamoris/runtime/inference.hpp", "flamoris/runtime/registered_adapter.hpp",
        }:
            failures.append(f"{relative}: composition/native implementation crossed kernel boundary: {include}")
    if relative.parts[1] == "domain" and any("support/" in include for include in includes):
        failures.append(f"{relative}: test support in production domain")
if failures:
    print("\n".join(failures), file=sys.stderr)
    sys.exit(1)
print("Source dependency boundaries: checked (behavioral acceptance requires tests)")
