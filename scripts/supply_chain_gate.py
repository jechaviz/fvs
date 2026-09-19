#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

USES_RE = re.compile(r"^\s*(?:-\s*)?uses:\s*([^\s#]+)", re.MULTILINE)
PIN_RE = re.compile(r"^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:/[A-Za-z0-9_./-]+)?@[0-9a-fA-F]{40}$")


def main() -> int:
    ap = argparse.ArgumentParser(description="Require immutable GitHub Action pins")
    ap.add_argument("--root", type=Path, default=Path("."))
    args = ap.parse_args()
    root = args.root.resolve()
    failures = []
    checked = []

    workflows = root / ".github" / "workflows"
    for path in sorted(workflows.glob("*.y*ml")):
        text = path.read_text(encoding="utf-8")
        for value in USES_RE.findall(text):
            if value.startswith("./"):
                continue
            checked.append({"workflow": str(path.relative_to(root)), "uses": value})
            if not PIN_RE.fullmatch(value):
                failures.append({"workflow": str(path.relative_to(root)), "uses": value})

    print(json.dumps({"ok": not failures, "checked": checked, "failures": failures}, separators=(",", ":"), sort_keys=True))
    if failures:
        print("Supply-chain gate failed: external actions must be pinned to immutable 40-hex commit SHAs.", file=__import__("sys").stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
