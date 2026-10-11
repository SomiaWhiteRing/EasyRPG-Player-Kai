"""Compare original and Player 3020 direct/indirect map self string sources."""

from __future__ import annotations

import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-string-self-source-manifest.json"
OUTPUT = EVIDENCE / "maniac-250921-string-self-source-audit.json"
ORIGINAL_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_SHA256 = "6519905607a31383a19b28e8cf3e40fd41ae6e524b86dbfa29897a44b844d954"
EXPECTED_STRINGS = ["Bank sample", "Literal", "Bank sample"]


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256 = parser["sha256"]
    ber = parser["ber"]
    chunks = parser["chunks"]
    decode_map_self_numbers = parser["decode_map_self_numbers"]
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest["save_variant"] != "string-self-source" or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA256:
        raise ValueError("wrong reference fixture")
    results = {}
    for label in ("original", "player"):
        stem = f"maniac-250921-string-self-source" if label == "original" else f"maniac-player-250921-string-self-source"
        trace_bytes = (EVIDENCE / f"{stem}-trace.json").read_bytes()
        trace = json.loads(trace_bytes)
        save_bytes = (EVIDENCE / f"{stem}-save.lsd").read_bytes()
        if (trace["reference_sha256"] != ORIGINAL_SHA256 or
                trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
                trace["save_file_sha256"] != sha256(save_bytes) or
                trace["save_file_length"] != len(save_bytes) or trace["save_slot"] != 1):
            raise ValueError(f"{label} execution/save binding failed")
        if label == "player" and (trace.get("player_sha256") != PLAYER_SHA256 or
                trace.get("player_arguments") != ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921"]):
            raise ValueError("Player executable or arguments differ")
        header_length, position = ber(save_bytes, 0)
        if save_bytes[position:position + header_length] != b"LcfSaveData":
            raise ValueError(f"invalid {label} LSD header")
        root = chunks(save_bytes, position + header_length, len(save_bytes))
        system = next(entry for entry in root if entry["id"] == 101)
        fields = chunks(save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])
        variables = next(entry for entry in fields if entry["id"] == 34)
        number_payload = save_bytes[variables["payload_offset"]:variables["payload_offset"] + variables["length"]]
        if len(number_payload) % 4:
            raise ValueError(f"unaligned {label} numeric vector")
        numbers = list(struct.unpack("<" + "i" * (len(number_payload) // 4), number_payload))
        if numbers[:3] != [0, 99, -1]:
            raise ValueError(f"{label} marker or indirect address differs: {numbers}")
        string_field = next(entry for entry in fields if entry["id"] == 36)
        payload = save_bytes[string_field["payload_offset"]:string_field["payload_offset"] + string_field["length"]]
        offset = 0
        decoded_strings = []
        unit_size = 2 if label == "original" else 1
        encoding = "utf-16-le" if label == "original" else "utf-8"
        while offset < len(payload):
            units, offset = ber(payload, offset)
            byte_length = units * unit_size
            if offset + byte_length > len(payload):
                raise ValueError(f"truncated {label} string vector")
            decoded_strings.append(payload[offset:offset + byte_length].decode(encoding))
            offset += byte_length
        if decoded_strings != EXPECTED_STRINGS:
            raise ValueError(f"{label} global string results differ: {decoded_strings}")
        extension = next(entry for entry in root if entry["id"] == 116)
        children = chunks(save_bytes, extension["payload_offset"], extension["payload_offset"] + extension["length"])
        map_child = next(entry for entry in children if entry["id"] == 2)
        banks = decode_map_self_numbers(save_bytes, map_child)
        if (len(banks) != 1 or banks[0]["map_id"] != 1 or len(banks[0]["events"]) != 1 or
                banks[0]["events"][0]["event_id"] != 1 or
                banks[0]["events"][0]["field_3_observed_utf16_slots"][0] != "Bank sample"):
            raise ValueError(f"{label} self string origin differs: {banks}")
        results[label] = {
            "runtime_sha256": ORIGINAL_SHA256 if label == "original" else PLAYER_SHA256,
            "trace_sha256": sha256(trace_bytes),
            "save_sha256": sha256(save_bytes),
            "saved_numeric_slots_1_to_3": numbers[:3],
            "saved_global_string_slots_1_to_3": decoded_strings,
            "saved_self_bank": banks,
            "global_string_field_encoding_observed": encoding,
        }
    result = {
        "manifest_sha256": sha256(manifest_bytes),
        "input_file_hashes": manifest["files"],
        "results": results,
        "scope": "3020 assignment source modes direct -1, indirect via V3=-1, and literal Text in one map event; global targets 1/3/2 and one 3036 self string; other 3020 operations/consumers remain unverified",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
