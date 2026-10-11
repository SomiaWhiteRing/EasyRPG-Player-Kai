"""Check a real Player 3002 save resumed by the 250921 original runtime."""

from __future__ import annotations

import hashlib
import json
import os
import runpy
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
SAVE = EVIDENCE / "maniac-250921-player-save-map-self-resume.lsd"
MANIFEST = EVIDENCE / "maniac-250921-load-resume-manifest.json"
TRACE = EVIDENCE / "maniac-250921-player-save-original-load-trace.json"
OUTPUT = EVIDENCE / "maniac-250921-player-save-original-load-audit.json"
RUNNER = ROOT / "build/windows-x64-vs2022-release-local/Release/test_runner.exe"
read = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def main() -> None:
    env = os.environ.copy()
    env["MANIAC_COMPARE_PLAYER_SAVE"] = str(SAVE)
    completed = subprocess.run(
        [str(RUNNER), "--no-colors=true",
         "--test-case=Maniac Player save frame preserves original map context"],
        cwd=ROOT, env=env, capture_output=True, text=True,
    )
    if completed.returncode or "Status: SUCCESS!" not in completed.stdout:
        raise RuntimeError(completed.stdout + completed.stderr)
    blob = SAVE.read_bytes()
    header_size, position = read["ber"](blob, 0)
    if blob[position:position + header_size] != b"LcfSaveData":
        raise ValueError("invalid Player LSD header")
    root = {entry["id"]: entry for entry in read["chunks"](blob, position + header_size, len(blob))}
    bank_chunk = root[116]
    bank_payload = blob[bank_chunk["payload_offset"]:bank_chunk["payload_offset"] + bank_chunk["length"]]
    child = next(entry for entry in read["chunks"](bank_payload, 0, len(bank_payload)) if entry["id"] == 2)
    decoded = read["decode_map_self_numbers"](bank_payload, child)
    if decoded != [{"map_id": 1, "events": [{"event_id": 1,
                    "numbers_positive_slots": [42, 0]}], "other_map_fields_hex": {}}]:
        raise ValueError(f"unexpected Player original-bank payload: {decoded}")
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace = json.loads(TRACE.read_text(encoding="utf-8"))
    expected = [0, 7, 0, 42, 42]
    if trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or (
            trace["reference_sha256"] != manifest["files"]["RPG_RT.exe"] or
            trace.get("input_save_sha256") != sha256(blob)):
        raise ValueError("original runtime trace is not bound to the Player save")
    if not any(sample["slots"] == expected and sample["vector_length"] == 6
               for sample in trace["samples"]):
        raise ValueError("original runtime did not resume Player's saved self value")
    result = {
        "player_save_sha256": sha256(blob),
        "player_save_length": len(blob),
        "root_0x74_sha256": sha256(bank_payload),
        "saved_map_event_numeric_bank": decoded,
        "original_load_fixture_manifest_sha256": sha256(manifest_bytes),
        "original_load_trace_sha256": sha256(TRACE.read_bytes()),
        "original_after_player_save_load_physical_numeric_slots": expected,
        "scope": "One Player.exe Maniac 3002 save loaded by the original 250921 runtime with one map self numeric continuation; other versions and states remain unverified",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
