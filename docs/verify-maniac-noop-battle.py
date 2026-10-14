"""Verify frozen 250921 battle-event 3024/3034/3035 evidence."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import runpy


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
FIXTURE = ROOT / "build/maniac-250921-noop-battle"
MANIFEST = EVIDENCE / "maniac-250921-noop-battle-manifest.json"
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
REFERENCE_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1",
               "--log-file", "noop-battle-player.log"]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def inspect(label: str, manifest_hash: str, executable: Path,
            expected_values: list[int], saved_values, ber, chunks) -> dict[str, object]:
    trace_file = EVIDENCE / f"maniac-250921-noop-battle-{label}-trace.json"
    save_file = EVIDENCE / f"maniac-250921-noop-battle-{label}-save.lsd"
    trace_bytes = trace_file.read_bytes()
    save_bytes = save_file.read_bytes()
    trace = json.loads(trace_bytes)
    if (trace["reference_sha256"] != REFERENCE_SHA256 or
            trace["runtime_sha256"] != sha256(executable.read_bytes()) or
            trace["runtime_arguments"][:3] + trace["runtime_arguments"][-2:] != ([] if label == "original" else PLAYER_ARGS) or
            trace["fixture_manifest_sha256"] != manifest_hash or
            trace["save_file_sha256"] != sha256(save_bytes) or
            trace["save_file_length"] != len(save_bytes) or
            trace["save_slot"] != 1):
        raise ValueError(f"{label} runtime/input/save provenance differs")
    values = saved_values(save_bytes, ber, chunks)
    if values[:10] != expected_values:
        raise ValueError(f"{label} battle no-op marker sequence differs: {values}")
    if len(values) != (10 if label == "original" else 11) or (
            label == "player" and values[10] != 0):
        raise ValueError(f"{label} variable vector length or trailing slot differs: {values}")
    if label == "original":
        expected = [(3024 if index % 3 == 0 else 3034 if index % 3 == 1 else 3035,
                     2 * index, 2 * index + (1 if index % 3 == 1 else 2))
                    for index in range(9)]
        events = trace["dispatch_events"]
        actual = [(event["opcode"], event["cursor_before"], event["cursor_after"])
                  for event in events]
        if (not trace["dispatch_instrumented"] or actual != expected or
                any(event["caller_rva"] != "0x8a13c" or event["owner_mode"] != 2 or
                    event["result_al"] != 1 for event in events)):
            raise ValueError(f"original battle dispatcher trace differs: {events}")
    elif trace["dispatch_instrumented"] or trace["dispatch_events"]:
        raise ValueError("Player trace unexpectedly claims original instrumentation")
    return {"trace_sha256": sha256(trace_bytes), "save_sha256": sha256(save_bytes),
            "global_numeric_ids_1_to_10": values[:10], "numeric_vector_length": len(values),
            "original_dispatch_events": trace["dispatch_events"] if label == "original" else None}


def main() -> None:
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    if (manifest["save_variant"] != "noop-battle" or
            manifest["expected_save_slot"] != 1 or
            manifest["files"]["RPG_RT.exe"] != REFERENCE_SHA256 or
            sha256(ORIGINAL.read_bytes()) != REFERENCE_SHA256):
        raise ValueError("fixed battle fixture or original runtime differs")
    for name, expected_hash in manifest["files"].items():
        if sha256((FIXTURE / name).read_bytes()) != expected_hash:
            raise ValueError(f"fixture input changed: {name}")
    expected_values = manifest["expected_global_slots_after_event"]
    if expected_values != list(range(11, 111, 11)):
        raise ValueError("unexpected battle marker definition")
    helpers = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    parser = runpy.run_path(str(ROOT / "docs/verify-maniac-noop-long-skip.py"))
    result = {label: inspect(label, sha256(manifest_bytes), executable, expected_values,
                             parser["saved_values"], helpers["ber"], helpers["chunks"])
              for label, executable in (("original", ORIGINAL), ("player", PLAYER))}
    audit = {
        "original_sha256": REFERENCE_SHA256,
        "player_sha256": sha256(PLAYER.read_bytes()),
        "manifest_sha256": sha256(manifest_bytes),
        "results": result,
        "scope": "Original 250921 and Player run one fixed troop page with standalone 3024, 3034 and 3035, each with zero, one and three parameters. Frida observes nine original battle dispatcher calls at RVA 0x8a13c, successful returns, and their cursor movement. Both saves contain the nine adjacent markers and the preceding map marker. Original and Player save vector lengths differ by one trailing Player zero slot. This verifies the command continuation and value order in battle; it does not establish general battle-save restoration or bitwise save interchange.",
    }
    output = EVIDENCE / "maniac-250921-noop-battle-audit.json"
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: original and Player battle markers {expected_values}")


if __name__ == "__main__":
    main()
