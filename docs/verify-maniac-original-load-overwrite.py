"""Verify original 250921 load replaces a mutated map self numeric bank."""

from __future__ import annotations

import json
import runpy
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-load-overwrite-manifest.json"
TRACE = EVIDENCE / "maniac-250921-load-overwrite-trace.json"
SAVE = EVIDENCE / "maniac-250921-original-save-load-overwrite.lsd"
OUTPUT = EVIDENCE / "maniac-250921-load-overwrite-audit.json"
REFERENCE_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256 = parser["sha256"]
    ber = parser["ber"]
    chunks = parser["chunks"]
    decode_map_self_numbers = parser["decode_map_self_numbers"]

    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace_bytes = TRACE.read_bytes()
    trace = json.loads(trace_bytes)
    save_bytes = SAVE.read_bytes()
    if manifest["save_variant"] != "load-overwrite" or manifest["expected_save_slot"] != 1:
        raise ValueError("unexpected fixture variant or save slot")
    if manifest["files"]["RPG_RT.exe"] != REFERENCE_SHA256 or (
            trace["reference_sha256"] != REFERENCE_SHA256 or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["save_file_sha256"] != sha256(save_bytes)):
        raise ValueError("fixture, original runtime, trace, or save hash mismatch")

    header_length, position = ber(save_bytes, 0)
    if save_bytes[position:position + header_length] != b"LcfSaveData":
        raise ValueError("not an LSD save")
    root_chunks = chunks(save_bytes, position + header_length, len(save_bytes))
    extension = next(entry for entry in root_chunks if entry["id"] == 116)
    children = chunks(save_bytes, extension["payload_offset"],
                      extension["payload_offset"] + extension["length"])
    if [(entry["id"], entry["length"]) for entry in children] != [(2, 14)]:
        raise ValueError("unexpected original self bank extension")
    banks = decode_map_self_numbers(save_bytes, children[0])
    expected_banks = [{"map_id": 1, "events": [{"event_id": 1,
                       "numbers_positive_slots": [42]}], "other_map_fields_hex": {}}]
    if banks != expected_banks:
        raise ValueError(f"saved bank is not 42: {banks}")

    samples = trace["samples"]
    if len(samples) < 28 or [s["input_enter_count"] for s in samples] != list(range(len(samples))):
        raise ValueError("incomplete original runtime trace")
    if any(s["global_switch_vector_length"] != 1 or s["global_switch_slots_raw"] != [0]
           for s in samples):
        raise ValueError("unexpected global switch state")
    states = {
        (1, (0, None, None, None, None)): "initial",
        (5, (0, 0, 0, 42, 0)): "read_42",
        (6, (0, 0, 0, 42, 43)): "read_43",
    }
    transitions = []
    for sample in samples:
        key = (sample["vector_length"], tuple(sample["slots"]))
        if key not in states:
            raise ValueError(f"unexpected original runtime state: {sample}")
        stage = states[key]
        if not transitions or transitions[-1]["stage"] != stage:
            transitions.append({"input_enter_count": sample["input_enter_count"], "stage": stage})
    if [entry["stage"] for entry in transitions[:5]] != [
            "initial", "read_42", "read_43", "read_42", "read_43"]:
        raise ValueError(f"load did not restore the saved self bank: {transitions}")

    result = {
        "reference_sha256": REFERENCE_SHA256,
        "manifest_sha256": sha256(manifest_bytes),
        "trace_sha256": sha256(trace_bytes),
        "save_sha256": sha256(save_bytes),
        "sample_count": len(samples),
        "transitions": transitions,
        "saved_banks": banks,
        "scope": "Original 250921 save 42, mutate self to 43, load same save, then read 42 again",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
