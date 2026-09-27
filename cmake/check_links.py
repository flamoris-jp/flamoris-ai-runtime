"""Check local Markdown file links; remote URLs and fragment IDs are separate concerns."""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]
errors = []
for path in root.rglob("*.md"):
    if any(part in {".git", "build", "third_party"} for part in path.parts):
        continue
    for link in re.findall(r"!?\[[^\]]*\]\(([^)]+)\)", path.read_text(encoding="utf-8")):
        target = link.split("#", 1)[0].split("?", 1)[0]
        if not target or "://" in target or target.startswith(("mailto:", "sandbox:")):
            continue
        if not (path.parent / target).exists():
            errors.append(f"{path.relative_to(root)}: {target}")
if errors:
    print("Missing local Markdown targets:\n" + "\n".join(errors))
    sys.exit(1)
print("Local Markdown file links are valid.")
