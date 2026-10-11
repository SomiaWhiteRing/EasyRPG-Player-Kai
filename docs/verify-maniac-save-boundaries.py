"""Verify fixed original/Player 250921 Save slot and short-record boundaries."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import runpy


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
FIXTURE = ROOT / "build/maniac-250921-save-boundaries"
MANIFEST = EVIDENCE / "maniac-250921-save-boundaries-manifest.json"
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
ORIGINAL_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921",
               "--log-file", "noop-battle-player.log"]
EXPECTED_PARAMETERS = [[0, 0, 1, 3], [0, -1, 1, -6], [0, 0, 0, 6], [], [0], [1],
                       [0, 0], [0, 0, 1], [2, 2, 1, 8], [1, -1, 1, -7], [0, 1],
                       [0, 2, 1], [0, 9, 0, 0]]
EXPECTED_SLOTS = [[], [], [], [], [], [], [], [], [1], [2], [1], [2], [9]]
EXPECTED_VALUES = [3, 1, 0, 0, 0, 88, 88, 1, 99, 1]


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def saved_banks(blob: bytes, helper) -> list[dict]:
    size, position = helper["ber"](blob, 0)
    if blob[position:position + size] != b"LcfSaveData":
        raise ValueError("unexpected save header")
    root = {entry["id"]: entry for entry in helper["chunks"](blob, position + size, len(blob))}
    extension = root[116]
    payload = blob[extension["payload_offset"]:extension["payload_offset"] + extension["length"]]
    child = next(entry for entry in helper["chunks"](payload, 0, len(payload)) if entry["id"] == 2)
    return helper["decode_map_self_numbers"](payload, child)


def main() -> None:
    manifest_blob = MANIFEST.read_bytes()
    manifest = json.loads(manifest_blob)
    if (manifest["save_variant"] != "save-boundaries" or manifest["expected_save_slot"] != 9 or
            manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA256 or
            sha256(ORIGINAL.read_bytes()) != ORIGINAL_SHA256):
        raise ValueError("fixed original or save-boundaries fixture changed")
    for name, expected_hash in manifest["files"].items():
        if sha256((FIXTURE / name).read_bytes()) != expected_hash:
            raise ValueError(f"fixture input changed: {name}")
    if manifest["expected_global_slots_after_event"] != EXPECTED_VALUES:
        raise ValueError("fixture marker definition changed")
    helper = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    decoder = runpy.run_path(str(ROOT / "docs/verify-maniac-noop-long-skip.py"))
    outputs = {}
    for label, executable in (("original", ORIGINAL), ("player", PLAYER)):
        trace_file = EVIDENCE / f"maniac-250921-save-boundaries-{label}-trace.json"
        trace_blob = trace_file.read_bytes()
        trace = json.loads(trace_blob)
        if (trace["reference_sha256"] != ORIGINAL_SHA256 or
                trace["runtime_sha256"] != sha256(executable.read_bytes()) or
                trace["runtime_arguments"] != ([] if label == "original" else PLAYER_ARGS) or
                trace["fixture_manifest_sha256"] != sha256(manifest_blob) or
                trace["save_slot"] != 9):
            raise ValueError(f"{label} trace provenance changed")
        saves = {}
        for slot in (1, 2, 9):
            filename = EVIDENCE / f"maniac-250921-save-boundaries-{label}-save{slot:02d}.lsd"
            blob = filename.read_bytes()
            if slot == 9:
                if (trace["save_file_sha256"] != sha256(blob) or
                        trace["save_file_length"] != len(blob)):
                    raise ValueError(f"{label} final save/trace mismatch")
            else:
                meta = trace["intermediate_saves"][str(slot)]
                if meta != {"sha256": sha256(blob), "length": len(blob)}:
                    raise ValueError(f"{label} intermediate slot {slot} changed")
            values = decoder["saved_values"](blob, helper["ber"], helper["chunks"])
            expected = EXPECTED_VALUES if slot == 9 else [3, 1, 0, 0, 0, 88, 88, 1, 0, 1]
            if values[:10] != expected or len(values) != (10 if label == "original" else 11):
                raise ValueError(f"{label} Save{slot:02d} markers differ: {values}")
            banks = saved_banks(blob, helper)
            if banks != [{"map_id": 1, "events": [{"event_id": 1,
                          "numbers_positive_slots": [2, 0, 0, 0, 0, 0, 1]}],
                          "other_map_fields_hex": {}}]:
                raise ValueError(f"{label} Save{slot:02d} self result differs: {banks}")
            saves[f"slot{slot:02d}"] = {"sha256": sha256(blob), "values": values[:10]}
        if label == "original":
            events = trace["dispatch_events"]
            if (not trace.get("save_instrumented") or
                    [event["parameters"] for event in events] != EXPECTED_PARAMETERS or
                    [event["save_slots"] for event in events] != EXPECTED_SLOTS or
                    events[0]["values_before"][3] != 99 or events[0]["values_after"][3] != 0 or
                    events[8]["values_before"][8] != 0 or events[8]["values_after"][8] != 1):
                raise ValueError("original Save handler branches or result timing changed")
        elif trace.get("save_instrumented") or trace["dispatch_events"]:
            raise ValueError("Player trace claims original instrumentation")
        outputs[label] = {"trace_sha256": sha256(trace_blob), "saves": saves}
    audit = {
        "original_sha256": ORIGINAL_SHA256, "player_sha256": sha256(PLAYER.read_bytes()),
        "fixture_manifest_sha256": sha256(manifest_blob), "results": outputs,
        "original_parameters": EXPECTED_PARAMETERS, "original_save_slots": EXPECTED_SLOTS,
        "scope": "Fixed original 250921 and Player execute thirteen standalone 3002 records on a map autostart page. Original handler traces verify zero through four argument records, invalid slots, one-lookup A0=2 global and A0=1 negative self slot sources, disabled/enabled result writes and positive-result timing. Both engines' saved global and self markers match at slots 1, 2 and 9. This closes only W11-018/019; filename/counter boundaries, frequency guard and cross-runtime restoration remain separate.",
    }
    output = EVIDENCE / "maniac-250921-save-boundaries-audit.json"
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: 13 original branches and 3 original/Player save slots verified")


if __name__ == "__main__":
    main()
