#!/usr/bin/env python3
"""Check local Markdown destinations without contacting external services."""
from pathlib import Path
import re
import sys
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parents[1]
LINK = re.compile(r"!?\[[^\]]*\]\(([^\s)]+)(?:\s+[^)]*)?\)")
failures = []
checked = 0
for path in sorted(ROOT.rglob("*.md")):
    relative = path.relative_to(ROOT)
    if any(part in {".git", "build", "third_party"} for part in relative.parts):
        continue
    text = re.sub(r"```[^\n]*\n.*?```", "", path.read_text(encoding="utf-8"), flags=re.S)
    for match in LINK.finditer(text):
        target = match.group(1).strip("<>")
        if target.startswith("#") or re.match(r"^[A-Za-z][A-Za-z0-9+.-]*:", target):
            continue
        target = unquote(target.split("#", 1)[0].split("?", 1)[0])
        if not target:
            continue
        resolved = (path.parent / target).resolve()
        if not resolved.is_relative_to(ROOT) or not resolved.exists():
            failures.append(f"{relative}: missing or escaping local destination {target}")
        checked += 1
if failures:
    print("\n".join(failures), file=sys.stderr)
    sys.exit(1)
print(f"Local Markdown destinations: {checked} checked")
