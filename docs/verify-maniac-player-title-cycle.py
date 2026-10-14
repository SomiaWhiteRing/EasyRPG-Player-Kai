"""Verify actual return-to-title/new-game clearing of Player map self bank."""

from __future__ import annotations

import hashlib
import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-title-cycle-save-manifest.json"
OUTPUT = EVIDENCE / "maniac-player-250921-title-cycle-audit.json"
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
ORIGINAL_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_SHA256 = "6b4d1f70ece4819871ed8138226ac2e4c252aea1226553040ddb9d058f4ebe94"
ARGS = ["--new-game", "--patch-maniac", "1"]


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def read_state(blob: bytes, helpers) -> tuple[dict[int, int], list[dict]]:
    ber, chunks = helpers["ber"], helpers["chunks"]
    length, position = ber(blob, 0)
    if blob[position:position + length] != b"LcfSaveData":
        raise ValueError("not a valid LSD save")
    roots = {entry["id"]: entry for entry in chunks(blob, position + length, len(blob))}
    system = roots[101]
    fields = {entry["id"]: entry for entry in chunks(
        blob, system["payload_offset"], system["payload_offset"] + system["length"])}
    values = {}
    if 34 in fields:
        field = fields[34]
        raw = blob[field["payload_offset"]:field["payload_offset"] + field["length"]]
        if len(raw) % 4:
            raise ValueError("malformed global numeric values")
        numbers = struct.unpack("<" + str(len(raw) // 4) + "i", raw)
        values = {index + 1: value for index, value in enumerate(numbers)}
    banks = []
    if 116 in roots:
        extension = roots[116]
        entries = chunks(blob, extension["payload_offset"], extension["payload_offset"] + extension["length"])
        bank = next((entry for entry in entries if entry["id"] == 2), None)
        if bank is not None:
            banks = helpers["decode_map_self_numbers"](blob, bank)
    return values, banks


def main() -> None:
    helpers = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    if (manifest["save_variant"] != "title-cycle-save" or
            manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA256 or
            sha256(ORIGINAL.read_bytes()) != ORIGINAL_SHA256 or
            sha256(PLAYER.read_bytes()) != PLAYER_SHA256):
        raise ValueError("fixed title-cycle input or executable changed")
    fixture = ROOT / "build/maniac-original-title-cycle-save"
    for name, expected in manifest["files"].items():
        if sha256((fixture / name).read_bytes()) != expected:
            raise ValueError(f"title-cycle fixture changed: {name}")
    result = {}
    for label, expected_runtime, expected_args in (("original", ORIGINAL_SHA256, []),
                                                   ("player", PLAYER_SHA256, ARGS)):
        directory = EVIDENCE / f"maniac-250921-title-cycle-save-{label}"
        trace_bytes = (directory / "trace.json").read_bytes()
        trace = json.loads(trace_bytes.decode("utf-8-sig"))
        events = trace["events"]
        if (trace["reference_sha256"] != ORIGINAL_SHA256 or
                trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
                trace["runtime_sha256"] != expected_runtime or
                trace["runtime_arguments"][:3] != expected_args or len(events) != 4 or
                [event["save_slot"] for event in events] != [1, 2, 1, 2] or
                [event["input_enter_count"] for event in events] != sorted(
                    event["input_enter_count"] for event in events)):
            raise ValueError(f"{label} did not complete two title/new-game save cycles")
        observations = []
        for index, event in enumerate(events):
            snapshot = directory / event["snapshot"]
            blob = snapshot.read_bytes()
            if event["sha256"] != sha256(blob) or event["length"] != len(blob):
                raise ValueError(f"{label} title snapshot changed: {index + 1}")
            values, banks = read_state(blob, helpers)
            if values.get(3, 0) != 0:
                raise ValueError(f"{label} retained old self on new game: {index + 1}")
            if index % 2 == 0:
                if banks or values.get(4, 0) != 0:
                    raise ValueError(f"{label} pre-write snapshot retained map self: {index + 1}")
            else:
                actual = [(map_bank["map_id"], event_bank["event_id"],
                           event_bank["numbers_positive_slots"][0])
                          for map_bank in banks for event_bank in map_bank["events"]]
                if values.get(4, 0) != 42 or actual != [(1, 1, 42)]:
                    raise ValueError(f"{label} post-write self differs: {index + 1}: {actual}")
            observations.append({"save_slot": event["save_slot"],
                                 "input_enter_count": event["input_enter_count"],
                                 "save_sha256": sha256(blob),
                                 "global_id_3": values.get(3, 0),
                                 "global_id_4": values.get(4, 0),
                                 "map_self_present": bool(banks)})
        result[label] = {"trace_sha256": sha256(trace_bytes), "observations": observations}
    audit = {
        "manifest_sha256": sha256(manifest_bytes),
        "original_sha256": ORIGINAL_SHA256,
        "player_sha256": PLAYER_SHA256,
        "runs": result,
        "scope": "Both fixed 250921 Windows original and Player complete two Save01/Save02 sequences across return-to-title and new game. Each pre-write save has global self read 0 and no map bank; each post-write save has read 42 and map 1/event 1 self 42. Other load/resume lifecycle paths remain separate requirements.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: two original and Player title/new-game cycles match")


if __name__ == "__main__":
    main()
