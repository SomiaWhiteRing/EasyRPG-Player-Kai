"""Verify the 250921 runtime clears self state across Return to Title/New Game."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-title-cycle-manifest.json"
TRACE = EVIDENCE / "maniac-250921-title-cycle-trace.json"
OUTPUT = EVIDENCE / "maniac-250921-title-cycle-audit.json"
REFERENCE_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def stage(sample: dict) -> str:
    slots = sample["slots"]
    length = sample["vector_length"]
    if length == 1 and slots == [0, None, None, None, None]:
        return "new_game_initial"
    if length == 5 and slots == [0, 0, 0, 0, 0]:
        return "read_before_write_zero"
    if length == 6 and slots == [0, 0, 0, 0, 42]:
        return "read_after_write_42"
    raise ValueError(f"unexpected title-cycle sample: {sample}")


def main() -> None:
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace = json.loads(TRACE.read_text(encoding="utf-8"))
    if manifest["save_variant"] != "title-cycle" or manifest["expected_global_slots_after_event"] != [0, 0, 0, 0, 42]:
        raise ValueError("unexpected title-cycle input")
    if manifest["files"]["RPG_RT.exe"] != REFERENCE_SHA256 or (
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes)) or (
            trace["reference_sha256"] != manifest["files"]["RPG_RT.exe"]):
        raise ValueError("title-cycle trace not bound to original input")
    samples = trace["samples"]
    if len(samples) < 37 or [sample["input_enter_count"] for sample in samples] != list(range(len(samples))):
        raise ValueError("title-cycle capture is incomplete")
    if any(sample["global_switch_vector_length"] != 1 or sample["global_switch_slots_raw"] != [0]
           for sample in samples):
        raise ValueError("unexpected global switch state")
    transitions = []
    for sample in samples:
        current = stage(sample)
        if not transitions or transitions[-1]["stage"] != current:
            transitions.append({"input_enter_count": sample["input_enter_count"], "stage": current})
    expected = ["new_game_initial", "read_before_write_zero", "read_after_write_42",
                "new_game_initial", "read_before_write_zero", "read_after_write_42",
                "new_game_initial", "read_before_write_zero"]
    if [entry["stage"] for entry in transitions] != expected:
        raise ValueError(f"unexpected title/new-game lifecycle: {transitions}")
    result = {
        "reference_sha256": trace["reference_sha256"],
        "manifest_sha256": sha256(manifest_bytes),
        "trace_sha256": sha256(TRACE.read_bytes()),
        "sample_count": len(samples),
        "transitions": transitions,
        "scope": "Return to Title followed by automatic New Game clears one map event numeric self bank in original 250921 runtime",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
