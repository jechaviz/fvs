#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path

BASELINE = {
    "core/src/core.c": {"lines": 2232, "bytes": 198806},
    "frontend/components/App.vue": {"lines": 291, "bytes": 58349},
    "frontend/css/base.css": {"lines": 40, "bytes": 46018},
    "shim-python/app.py": {"lines": 417, "bytes": 34442},
    "shim-php/index.php": {"lines": 82, "bytes": 24683},
}


def measure(path: Path) -> dict[str, int]:
    data = path.read_bytes()
    return {"lines": len(data.splitlines()), "bytes": len(data)}


def main() -> int:
    ap = argparse.ArgumentParser(description="FVS architecture growth ratchet")
    ap.add_argument("--root", type=Path, default=Path("."))
    args = ap.parse_args()
    root = args.root.resolve()

    failures = []
    measured = {}
    for rel, limit in BASELINE.items():
        path = root / rel
        if not path.is_file():
            failures.append({"path": rel, "reason": "missing"})
            continue
        actual = measure(path)
        measured[rel] = actual
        for metric in ("lines", "bytes"):
            if actual[metric] > limit[metric]:
                failures.append({
                    "path": rel,
                    "metric": metric,
                    "actual": actual[metric],
                    "baseline": limit[metric],
                    "delta": actual[metric] - limit[metric],
                })

    payload = {"ok": not failures, "measured": measured, "failures": failures}
    print(json.dumps(payload, separators=(",", ":"), sort_keys=True))
    if failures:
        print(
            "Architecture ratchet failed: extract responsibility instead of growing a hotspot, "
            "or intentionally lower/re-baseline after a reviewed decomposition.",
            file=__import__("sys").stderr,
        )
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
