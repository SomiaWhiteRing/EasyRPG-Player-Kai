"""Audit one 250921 original-runner load of a Player-encoded self bank."""

from __future__ import annotations

import hashlib
import json
import os
import runpy
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
SOURCE = EVIDENCE / "maniac-250921-original-save-map-self-resume.lsd"
EXPORTED = EVIDENCE / "maniac-250921-exported-save-map-self-modified.lsd"
MANIFEST = EVIDENCE / "maniac-250921-load-export-manifest.json"
TRACE = EVIDENCE / "maniac-250921-load-export-trace.json"
OUTPUT = EVIDENCE / "maniac-250921-original-bank-export-audit.json"
REPRODUCED = ROOT / "build/maniac-original-save-map-self-export-check.lsd"
RUNNER = ROOT / "build/windows-x64-vs2022-release-local/Release/test_runner.exe"
read = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def bank_payload(blob: bytes) -> bytes:
    header_size, position = read["ber"](blob, 0)
    if blob[position:position + header_size] != b"LcfSaveData":
        raise ValueError("invalid LSD header")
    root = read["chunks"](blob, position + header_size, len(blob))
    extension = next(entry for entry in root if entry["id"] == 116)
    start = extension["payload_offset"]
    return blob[start:start + extension["length"]]


def main() -> None:
    env = os.environ.copy()
    env["MANIAC_READ_ORIGINAL_EXPORT_SOURCE"] = str(SOURCE)
    env["MANIAC_WRITE_ORIGINAL_BANK_EXPORT"] = str(REPRODUCED)
    completed = subprocess.run(
        [str(RUNNER), "--no-colors=true",
         "--test-case=Maniac observed self bank exports a changed value to original format"],
        cwd=ROOT, env=env, capture_output=True, text=True,
    )
    if completed.returncode or "Status: SUCCESS!" not in completed.stdout:
        raise RuntimeError(completed.stdout + completed.stderr)
    source, exported = SOURCE.read_bytes(), EXPORTED.read_bytes()
    if REPRODUCED.read_bytes() != exported:
        raise ValueError("exported LSD is not reproducible")
    original_bank, changed_bank = bank_payload(source), bank_payload(exported)
    differences = [index for index, (left, right) in enumerate(zip(original_bank, changed_bank))
                   if left != right]
    if len(original_bank) != len(changed_bank) or differences != [10] or (
            original_bank[10], changed_bank[10]) != (42, 43):
        raise ValueError("export changed bytes outside observed self slot 1")
    child = read["chunks"](changed_bank, 0, len(changed_bank))[0]
    decoded = read["decode_map_self_numbers"](changed_bank, child)
    if decoded != [{"map_id": 1, "events": [{"event_id": 1, "numbers_positive_slots": [43]}],
                    "other_map_fields_hex": {}}]:
        raise ValueError("exported bank did not contain map 1 event 1 self value 43")
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace = json.loads(TRACE.read_text(encoding="utf-8"))
    expected = [0, 7, 0, 42, 43]
    if manifest["save_variant"] != "load-export" or manifest["expected_global_slots_after_event"] != expected:
        raise ValueError("unexpected original load fixture")
    if trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or (
            trace["reference_sha256"] != manifest["files"]["RPG_RT.exe"] or
            trace.get("input_save_sha256") != sha256(exported)):
        raise ValueError("export load trace is not bound to its fixture and input")
    matching = [sample for sample in trace["samples"] if sample["slots"] == expected
                and sample["vector_length"] == 6]
    if not matching:
        raise ValueError("original runtime did not read exported self value 43")
    audit = {
        "source_save_sha256": sha256(source),
        "exported_save_sha256": sha256(exported),
        "original_bank_sha256": sha256(original_bank),
        "exported_bank_sha256": sha256(changed_bank),
        "bank_changed_offset": differences[0],
        "bank_changed_values": [42, 43],
        "fixture_manifest_sha256": sha256(manifest_bytes),
        "original_trace_sha256": sha256(TRACE.read_bytes()),
        "original_after_load_physical_numeric_slots": expected,
        "scope": "One Player-encoded observed 0x74 map event numeric bank loaded by the original 250921 runtime; regular Player saving is not yet wired to this exporter",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(audit, indent=2))


if __name__ == "__main__":
    main()
