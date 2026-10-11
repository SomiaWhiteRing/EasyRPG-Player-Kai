"""Verify that liblcf retains the frozen original Maniac 0x74 save payload.

Build liblcf and test_runner first. This verifies byte preservation of one
extension field; it does not claim save compatibility or runtime bank restore.
"""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = ROOT / "docs/evidence/maniac-250921-original-save01.lsd"
ROUNDTRIP = ROOT / "build/maniac-original-save-preserved.lsd"
RUNNER = ROOT / "build/windows-x64-vs2022-release-local/Release/test_runner.exe"
EVIDENCE = ROOT / "docs/evidence/maniac-250921-original-raw-preservation.json"
audit = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))


def payload(path: Path) -> tuple[bytes, bytes]:
    blob = path.read_bytes()
    header_size, position = audit["ber"](blob, 0)
    if blob[position:position + header_size] != b"LcfSaveData":
        raise ValueError(f"invalid LSD header: {path}")
    root = audit["chunks"](blob, position + header_size, len(blob))
    extension = next((entry for entry in root if entry["id"] == 116), None)
    if extension is None:
        raise ValueError(f"original bank extension absent: {path}")
    start = extension["payload_offset"]
    return blob, blob[start:start + extension["length"]]


def main() -> None:
    env = os.environ.copy()
    env["MANIAC_READ_ORIGINAL_SAVE"] = str(ORIGINAL)
    env["MANIAC_WRITE_ORIGINAL_SAVE_ROUNDTRIP"] = str(ROUNDTRIP)
    completed = subprocess.run(
        [str(RUNNER), "--no-colors=true",
         "--test-case=Maniac original save fixture exposes serialized state"],
        cwd=ROOT, env=env, capture_output=True, text=True,
    )
    if completed.returncode or "Status: SUCCESS!" not in completed.stdout:
        raise RuntimeError(completed.stdout + completed.stderr)
    original, before = payload(ORIGINAL)
    rewritten, after = payload(ROUNDTRIP)
    if before != after or len(before) != 53:
        raise ValueError("original 0x74 payload changed across liblcf round trip")
    result = {
        "original_lsd_sha256": hashlib.sha256(original).hexdigest(),
        "rewritten_lsd_sha256": hashlib.sha256(rewritten).hexdigest(),
        "original_lsd_length": len(original),
        "rewritten_lsd_length": len(rewritten),
        "root_0x74_payload_length": len(before),
        "root_0x74_payload_sha256": hashlib.sha256(before).hexdigest(),
        "root_0x74_payload_identical": True,
        "test": "Maniac original save fixture exposes serialized state",
        "scope": "Raw 0x74 payload preservation only; no Player runtime restore or whole-file identity",
    }
    EVIDENCE.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
