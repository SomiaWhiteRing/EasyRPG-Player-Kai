"""Verify the bounded Player map-autostart result for standalone opcode 3024."""

from __future__ import annotations

import hashlib
import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-reserved2-noop-manifest.json"
TRACE = EVIDENCE / "maniac-player-250921-reserved2-noop-trace.json"
SAVE = EVIDENCE / "maniac-player-250921-reserved2-noop-save.lsd"
ORIGINAL_AUDIT = EVIDENCE / "maniac-250921-3024-scope-audit.json"
OUTPUT = EVIDENCE / "maniac-player-250921-3024-map-audit.json"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
PLAYER_SHA256 = "6b4d1f70ece4819871ed8138226ac2e4c252aea1226553040ddb9d058f4ebe94"
ARGS = ["--new-game", "--patch-maniac", "1"]


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def main() -> None:
    helpers = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    ber, chunks = helpers["ber"], helpers["chunks"]
    manifest_bytes = MANIFEST.read_bytes()
    trace_bytes = TRACE.read_bytes()
    save_bytes = SAVE.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace = json.loads(trace_bytes.decode("utf-8-sig"))
    original = json.loads(ORIGINAL_AUDIT.read_text(encoding="utf-8"))
    if (manifest["save_variant"] != "reserved2-noop" or
            original["manifest_sha256"] != sha256(manifest_bytes) or
            original["original_marker_global_numeric_id_1"] != 99 or
            trace["reference_sha256"] != manifest["files"]["RPG_RT.exe"] or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["player_sha256"] != PLAYER_SHA256 or
            trace["player_arguments"][:len(ARGS)] != ARGS or
            trace["save_file_sha256"] != sha256(save_bytes) or
            trace["save_file_length"] != len(save_bytes) or
            trace["save_slot"] != 1 or
            sha256(PLAYER.read_bytes()) != PLAYER_SHA256):
        raise ValueError("Player 3024 map fixture, runtime or saved output changed")
    header_length, position = ber(save_bytes, 0)
    if save_bytes[position:position + header_length] != b"LcfSaveData":
        raise ValueError("not a Player LSD save")
    root = chunks(save_bytes, position + header_length, len(save_bytes))
    system = next(entry for entry in root if entry["id"] == 101)
    fields = {entry["id"]: entry for entry in chunks(
        save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])}
    count = fields[33]
    count_payload = save_bytes[count["payload_offset"]:count["payload_offset"] + count["length"]]
    values = fields[34]
    number_payload = save_bytes[values["payload_offset"]:values["payload_offset"] + values["length"]]
    if not count_payload or count_payload[0] < 1 or len(number_payload) < 4 or len(number_payload) % 4:
        raise ValueError("Player global numeric vector is absent or malformed")
    if struct.unpack_from("<i", number_payload)[0] != 99:
        raise ValueError("Player 3024 did not advance to the variable marker")
    audit = {
        "fixture_manifest_sha256": sha256(manifest_bytes),
        "original_scope_audit_sha256": sha256(ORIGINAL_AUDIT.read_bytes()),
        "player_sha256": PLAYER_SHA256,
        "player_trace_sha256": sha256(trace_bytes),
        "player_save_sha256": sha256(save_bytes),
        "player_arguments": trace["player_arguments"],
        "player_marker_global_numeric_id_1": 99,
        "scope": "Player 250921 Windows map-autostart event executes independent 3024 with nonempty Args/Text, reaches the later ControlVars 99 marker and Maniac_Save. This matches the bounded original observable marker; timing and parallel/common/battle contexts remain open under W02-012.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: Player map-autostart 3024 advances to marker 99 and save")


if __name__ == "__main__":
    main()
