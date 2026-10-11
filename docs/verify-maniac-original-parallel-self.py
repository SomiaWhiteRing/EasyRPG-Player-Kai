"""Verify two original 250921 parallel map events retain separate self banks."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-parallel-self-manifest.json"
TRACE = EVIDENCE / "maniac-250921-parallel-self-trace.json"
OUTPUT = EVIDENCE / "maniac-250921-parallel-self-audit.json"
REFERENCE_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def main() -> None:
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace_bytes = TRACE.read_bytes()
    trace = json.loads(trace_bytes)
    expected_final = [0, 0, 0, 42, 43, 42, 43]
    if manifest["save_variant"] != "parallel-self" or (
            manifest["files"]["RPG_RT.exe"] != REFERENCE_SHA256 or
            manifest["expected_global_slots_after_event"] != expected_final or
            manifest["expected_global_numeric_vector_length_after_event"] != 8):
        raise ValueError("unexpected parallel fixture")
    if trace["reference_sha256"] != REFERENCE_SHA256 or (
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes)):
        raise ValueError("original reference or fixture hash mismatch")
    samples = trace["samples"]
    if len(samples) < 20 or [s["input_enter_count"] for s in samples] != list(range(len(samples))):
        raise ValueError("incomplete parallel trace")
    first = [0, 0, 0, 0, 0, 42, 43]
    transitions = []
    for sample in samples:
        if sample["vector_length"] != 8 or sample["slots"] not in (first, expected_final):
            raise ValueError(f"unexpected parallel bank observation: {sample}")
        if sample["global_switch_vector_length"] != 1 or sample["global_switch_slots_raw"] != [0]:
            raise ValueError("unexpected global switch state")
        stage = "first_parallel_pass" if sample["slots"] == first else "subsequent_parallel_pass"
        if not transitions or transitions[-1]["stage"] != stage:
            transitions.append({"input_enter_count": sample["input_enter_count"], "stage": stage})
    if [item["stage"] for item in transitions] != ["first_parallel_pass", "subsequent_parallel_pass"]:
        raise ValueError(f"parallel self banks did not persist independently: {transitions}")
    result = {
        "reference_sha256": REFERENCE_SHA256,
        "manifest_sha256": sha256(manifest_bytes),
        "trace_sha256": sha256(trace_bytes),
        "sample_count": len(samples),
        "transitions": transitions,
        "scope": "Two parallel map events independently read prior self 0, write 42/43, then read 42/43 on the next pass",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
