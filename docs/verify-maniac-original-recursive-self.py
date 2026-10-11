"""Verify original 250921 recursive common frames share one self bank."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-recursive-self-manifest.json"
TRACE = EVIDENCE / "maniac-250921-recursive-self-trace.json"
OUTPUT = EVIDENCE / "maniac-250921-recursive-self-audit.json"
REFERENCE_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def main() -> None:
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace_bytes = TRACE.read_bytes()
    trace = json.loads(trace_bytes)
    expected = [0, 0, 0, 1, 42, 43]
    if manifest["save_variant"] != "recursive-self" or (
            manifest["files"]["RPG_RT.exe"] != REFERENCE_SHA256 or
            manifest["expected_global_slots_after_event"] != expected or
            manifest["expected_global_numeric_vector_length_after_event"] != 7):
        raise ValueError("unexpected recursive fixture")
    if trace["reference_sha256"] != REFERENCE_SHA256 or (
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes)):
        raise ValueError("original reference or fixture hash mismatch")
    samples = trace["samples"]
    if len(samples) < 8 or [s["input_enter_count"] for s in samples] != list(range(len(samples))):
        raise ValueError("incomplete recursive trace")
    stages = []
    for sample in samples:
        if sample["vector_length"] == 1 and sample["slots"] == [0, None, None, None, None, None]:
            stage = "before_call"
        elif sample["vector_length"] == 7 and sample["slots"] == expected:
            stage = "inner_read_42_outer_read_43"
        else:
            raise ValueError(f"unexpected recursive self observation: {sample}")
        if sample["global_switch_vector_length"] != 1 or sample["global_switch_slots_raw"] != [0]:
            raise ValueError("unexpected global switch state")
        if not stages or stages[-1]["stage"] != stage:
            stages.append({"input_enter_count": sample["input_enter_count"], "stage": stage})
    if [entry["stage"] for entry in stages] != ["before_call", "inner_read_42_outer_read_43"]:
        raise ValueError(f"recursive frame observations missing: {stages}")
    result = {
        "reference_sha256": REFERENCE_SHA256,
        "manifest_sha256": sha256(manifest_bytes),
        "trace_sha256": sha256(trace_bytes),
        "sample_count": len(samples),
        "transitions": stages,
        "scope": "Same common event reenters once; inner frame reads 42 and writes 43, outer frame reads 43",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
