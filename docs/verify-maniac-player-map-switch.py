"""Verify Player self-bank identity after an actual 250921 map transfer."""

from __future__ import annotations

import hashlib
import json
import runpy
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
OUTPUT = EVIDENCE / "maniac-player-250921-map-switch-audit.json"
MANIFEST = EVIDENCE / "maniac-250921-save-map-switch-manifest.json"
TRACE = EVIDENCE / "maniac-player-250921-map-switch-trace.json"
SAVE = EVIDENCE / "maniac-player-250921-map-switch-save.lsd"
ORIGINAL_SAVE = EVIDENCE / "maniac-250921-original-save-map-switch.lsd"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
PLAYER_SHA256 = "6b4d1f70ece4819871ed8138226ac2e4c252aea1226553040ddb9d058f4ebe94"
ORIGINAL_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921"]


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def read_banks(blob: bytes, ber, chunks, decode_map_self_numbers) -> tuple[int, list[dict]]:
    length, position = ber(blob, 0)
    if blob[position:position + length] != b"LcfSaveData":
        raise ValueError("invalid save header")
    roots = {entry["id"]: entry for entry in chunks(blob, position + length, len(blob))}
    party = roots[104]
    fields = {entry["id"]: entry for entry in chunks(
        blob, party["payload_offset"], party["payload_offset"] + party["length"])}
    map_field = fields[11]
    map_id, end = ber(blob[map_field["payload_offset"]:map_field["payload_offset"] + map_field["length"]], 0)
    if end != map_field["length"]:
        raise ValueError("invalid party map field")
    extension = roots[116]
    subfields = chunks(blob, extension["payload_offset"], extension["payload_offset"] + extension["length"])
    map_banks = decode_map_self_numbers(blob, next(entry for entry in subfields if entry["id"] == 2))
    return map_id, map_banks


def main() -> None:
    helpers = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace_bytes = TRACE.read_bytes()
    trace = json.loads(trace_bytes.decode("utf-8-sig"))
    save_bytes = SAVE.read_bytes()
    fixture = ROOT / "build/maniac-original-save-map-switch"
    if manifest["save_variant"] != "map-switch" or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA256:
        raise ValueError("wrong original transfer fixture")
    for name, expected in manifest["files"].items():
        if sha256((fixture / name).read_bytes()) != expected:
            raise ValueError(f"transfer fixture changed: {name}")
    if (trace["reference_sha256"] != ORIGINAL_SHA256 or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["player_sha256"] != PLAYER_SHA256 or
            trace["player_arguments"] != ARGS or
            trace["save_file_sha256"] != sha256(save_bytes) or
            trace["save_file_length"] != len(save_bytes) or
            trace["save_slot"] != 1 or sha256(PLAYER.read_bytes()) != PLAYER_SHA256):
        raise ValueError("Player transfer run not bound to fixed input and save")
    read = lambda blob: read_banks(blob, helpers["ber"], helpers["chunks"], helpers["decode_map_self_numbers"])
    original_map, original_banks = read(ORIGINAL_SAVE.read_bytes())
    player_map, player_banks = read(save_bytes)
    if original_map != player_map or player_map != 2:
        raise ValueError("Player did not save after actual transfer to map 2")
    expected = [(1, 1, 42), (2, 2, 43)]
    for banks in (original_banks, player_banks):
        observed = [(entry["map_id"], event["event_id"], event["numbers_positive_slots"][0])
                    for entry in banks for event in entry["events"]]
        if observed != expected:
            raise ValueError(f"map/event self identities differ: {observed}")
    audit = {
        "manifest_sha256": sha256(manifest_bytes),
        "original_save_sha256": sha256(ORIGINAL_SAVE.read_bytes()),
        "player_trace_sha256": sha256(trace_bytes),
        "player_save_sha256": sha256(save_bytes),
        "player_sha256": PLAYER_SHA256,
        "saved_party_map_id": player_map,
        "map_event_self_first_values": expected,
        "scope": "Fixed original and Player 250921 Windows map transfer from map 1 to map 2 retains map 1/event 1 self 42 and map 2/event 2 self 43. Trailing numeric allocation slots differ and are not counted as observed self values. Return-to-title/new-game behavior remains W01-017 work.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: Player transfer preserves distinct map self-bank identities")


if __name__ == "__main__":
    main()
