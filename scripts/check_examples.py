#!/usr/bin/env python3
"""Validate every documented request and response example."""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
for path in sorted((root / "examples").glob("*.json")):
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise SystemExit(f"{path}: top-level value is not an object")
print("example JSON files: PASS")
