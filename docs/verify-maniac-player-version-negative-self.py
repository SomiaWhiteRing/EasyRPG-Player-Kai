"""Verify Player's explicit Maniac release selection against original saves."""

from __future__ import annotations

import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
OUTPUT = EVIDENCE / "maniac-player-version-negative-self-audit.json"
PLAYER_SHA256 = "fd80779036ac90bb288bfe1be0320edd8ddddbfb676accef15aa8bfa0b39a5e2"


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256 = parser["sha256"]
    ber = parser["ber"]
    chunks = parser["chunks"]
    decode_map_self_numbers = parser["decode_map_self_numbers"]
    versions = {}
    for version, expected in (("241028", [0, 99]), ("250921", [42, 99])):
        original_audit = json.loads((EVIDENCE / "maniac-241028-negative-self-audit.json").read_text())
        original_result = original_audit["versions"][version]
        manifest_bytes = (EVIDENCE / f"maniac-{version}-negative-self-manifest.json").read_bytes()
        trace_bytes = (EVIDENCE / f"maniac-player-{version}-negative-self-trace.json").read_bytes()
        trace = json.loads(trace_bytes)
        save_bytes = (EVIDENCE / f"maniac-player-{version}-negative-self-save.lsd").read_bytes()
        if (trace["player_sha256"] != PLAYER_SHA256 or
                trace["reference_sha256"] != original_result["reference_sha256"] or
                trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
                trace["save_file_sha256"] != sha256(save_bytes) or
                trace["save_file_length"] != len(save_bytes) or
                trace["save_slot"] != 1 or
                trace["player_arguments"] != ["--new-game", "--patch-maniac", "1", "--maniac-version", version]):
            raise ValueError(f"Player execution binding failed: {version}")
        header_length, position = ber(save_bytes, 0)
        if save_bytes[position:position + header_length] != b"LcfSaveData":
            raise ValueError(f"invalid Player save header: {version}")
        root = chunks(save_bytes, position + header_length, len(save_bytes))
        system = next(entry for entry in root if entry["id"] == 101)
        fields = chunks(save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])
        variables = next(entry for entry in fields if entry["id"] == 34)
        payload = save_bytes[variables["payload_offset"]:variables["payload_offset"] + variables["length"]]
        if len(payload) % 4:
            raise ValueError(f"unaligned Player variables: {version}")
        values = list(struct.unpack("<" + "i" * (len(payload) // 4), payload))
        if values[:2] != expected:
            raise ValueError(f"Player variable mismatch: {version}: {values}")
        self_chunks = [entry for entry in root if entry["id"] == 116]
        if version == "241028":
            if self_chunks:
                raise ValueError("241028 Player unexpectedly wrote a map self bank")
            bank = []
        else:
            if len(self_chunks) != 1:
                raise ValueError("250921 Player map self bank missing")
            extension = self_chunks[0]
            children = chunks(save_bytes, extension["payload_offset"], extension["payload_offset"] + extension["length"])
            bank = [decode_map_self_numbers(save_bytes, child) for child in children if child["id"] == 2]
            if (len(bank) != 1 or len(bank[0]) != 1 or bank[0][0]["map_id"] != 1 or
                    len(bank[0][0]["events"]) != 1 or bank[0][0]["events"][0]["event_id"] != 1 or
                    bank[0][0]["events"][0]["numbers_positive_slots"][0] != 42):
                raise ValueError(f"250921 Player map self bank mismatch: {bank}")
        if values[:2] != original_result["saved_global_variables_1_and_2"]:
            raise ValueError(f"Player and original semantic results differ: {version}")
        versions[version] = {
            "player_sha256": PLAYER_SHA256,
            "trace_sha256": sha256(trace_bytes),
            "save_sha256": sha256(save_bytes),
            "saved_global_variables_1_and_2": values[:2],
            "saved_map_self_banks": bank,
        }
    result = {
        "versions": versions,
        "scope": "Player CLI-selected release reproduces the observed negative direct ControlVars target and subsequent read/save results; Player save vectors may include trailing zero slots.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
