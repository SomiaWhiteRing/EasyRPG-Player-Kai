"""Audit an original 250921 save loaded after a liblcf read/write cycle."""

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
REWRITTEN = EVIDENCE / "maniac-250921-rewritten-save-map-self-resume.lsd"
MANIFEST = EVIDENCE / "maniac-250921-load-resume-manifest.json"
TRACES = {
    "original": EVIDENCE / "maniac-250921-load-resume-original-trace.json",
    "rewritten": EVIDENCE / "maniac-250921-load-resume-rewritten-trace.json",
}
OUTPUT = EVIDENCE / "maniac-250921-original-rewrite-load-audit.json"
RUNNER = ROOT / "build/windows-x64-vs2022-release-local/Release/test_runner.exe"
REPRODUCED = ROOT / "build/maniac-original-save-map-self-resume-roundtrip-check.lsd"
read = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def root_74(blob: bytes) -> bytes:
    header_length, position = read["ber"](blob, 0)
    if blob[position:position + header_length] != b"LcfSaveData":
        raise ValueError("invalid LSD header")
    root = read["chunks"](blob, position + header_length, len(blob))
    extension = next(entry for entry in root if entry["id"] == 116)
    start = extension["payload_offset"]
    return blob[start:start + extension["length"]]


def main() -> None:
    source = SOURCE.read_bytes()
    rewritten = REWRITTEN.read_bytes()
    env = os.environ.copy()
    env["MANIAC_READ_ORIGINAL_BANK_SAMPLE"] = str(SOURCE)
    env["MANIAC_WRITE_ORIGINAL_BANK_ROUNDTRIP"] = str(REPRODUCED)
    completed = subprocess.run(
        [str(RUNNER), "--no-colors=true",
         "--test-case=Maniac original map bank samples import observed sparse strings"],
        cwd=ROOT, env=env, capture_output=True, text=True,
    )
    if completed.returncode or "Status: SUCCESS!" not in completed.stdout:
        raise RuntimeError(completed.stdout + completed.stderr)
    if REPRODUCED.read_bytes() != rewritten:
        raise ValueError("frozen rewritten LSD is not reproducible from the original sample")
    bank = root_74(source)
    if len(bank) != 17 or bank != root_74(rewritten) or source == rewritten:
        raise ValueError("original 0x74 bank was changed or the LSD was not rewritten")
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    expected = [0, 7, 0, 42, 42]
    if manifest["save_variant"] != "load-resume" or manifest["expected_global_slots_after_event"] != expected:
        raise ValueError("unexpected load fixture")
    observations = {}
    input_hashes = {"original": sha256(source), "rewritten": sha256(rewritten)}
    for label, path in TRACES.items():
        trace = json.loads(path.read_text(encoding="utf-8"))
        if trace.get("input_save_sha256") != input_hashes[label]:
            raise ValueError(f"{label} trace does not identify its input save")
        if trace["fixture_manifest_sha256"] != sha256(manifest_bytes):
            raise ValueError(f"{label} trace manifest mismatch")
        if trace["reference_sha256"] != manifest["files"]["RPG_RT.exe"]:
            raise ValueError(f"{label} reference runtime mismatch")
        matching = [sample for sample in trace["samples"]
                    if sample["slots"] == expected and sample["vector_length"] == 6
                    and sample["global_switch_slots_raw"] == [0]
                    and sample["global_switch_vector_length"] == 1]
        if not matching:
            raise ValueError(f"{label} original runtime did not resume the self read")
        observations[label] = {"trace_sha256": sha256(path.read_bytes()),
                               "first_matching_input_enter_count": matching[0]["input_enter_count"]}
    audit = {
        "original_save_sha256": sha256(source),
        "rewritten_save_sha256": sha256(rewritten),
        "original_save_length": len(source),
        "rewritten_save_length": len(rewritten),
        "root_0x74_sha256": sha256(bank),
        "manifest_sha256": sha256(manifest_bytes),
        "original_runtime_observations": observations,
        "expected_physical_numeric_slots_after_load": expected,
        "scope": "One original save rewritten by liblcf then loaded by the original 250921 runtime; Player-authored save interoperability is separate",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(audit, indent=2))


if __name__ == "__main__":
    main()
