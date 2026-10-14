"""Check original and Player 3020 PopLine CRLF, no-LF and alias results."""

from __future__ import annotations

import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
OUTPUT = EVIDENCE / "maniac-250921-string-pop-line-audit.json"
ORIGINAL_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_SHA256 = "1c29d2181025586cc74b04d23f1315d5509a8bcb83411269ac0be6c42e78d0f8"


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256, ber, chunks = (parser[name] for name in ("sha256", "ber", "chunks"))
    manifest_bytes = (EVIDENCE / "maniac-250921-string-pop-line-manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest["save_variant"] != "string-pop-line" or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA256:
        raise ValueError("wrong original fixture")
    results = {}
    for label in ("original", "player"):
        stem = f"maniac-250921-string-pop-line" if label == "original" else f"maniac-player-250921-string-pop-line"
        trace_bytes = (EVIDENCE / f"{stem}-trace.json").read_bytes()
        trace = json.loads(trace_bytes)
        save_bytes = (EVIDENCE / f"{stem}-save.lsd").read_bytes()
        if (trace["reference_sha256"] != ORIGINAL_SHA256 or
                trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
                trace["save_file_sha256"] != sha256(save_bytes) or
                trace["save_file_length"] != len(save_bytes) or trace["save_slot"] != 1):
            raise ValueError(f"{label} input/output binding failed")
        if label == "player" and (trace.get("player_sha256") != PLAYER_SHA256 or
                trace.get("player_arguments", [])[:3] != ["--new-game", "--patch-maniac", "1"]):
            raise ValueError("Player executable or arguments changed")
        header_length, position = ber(save_bytes, 0)
        if save_bytes[position:position + header_length] != b"LcfSaveData":
            raise ValueError(f"invalid {label} save header")
        root = chunks(save_bytes, position + header_length, len(save_bytes))
        system = next(entry for entry in root if entry["id"] == 101)
        fields = chunks(save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])
        variables = next(entry for entry in fields if entry["id"] == 34)
        number_payload = save_bytes[variables["payload_offset"]:variables["payload_offset"] + variables["length"]]
        if len(number_payload) % 4 or struct.unpack_from("<i", number_payload)[0] != 99:
            raise ValueError(f"{label} later marker missing")
        strings = next(entry for entry in fields if entry["id"] == 36)
        payload = save_bytes[strings["payload_offset"]:strings["payload_offset"] + strings["length"]]
        offset = 0
        values = []
        unit_bytes = 2 if label == "original" else 1
        encoding = "utf-16-le" if label == "original" else "utf-8"
        while offset < len(payload):
            count, offset = ber(payload, offset)
            if count == (1 << 35) - 1:
                values.append(None)
                continue
            byte_length = count * unit_bytes
            if offset + byte_length > len(payload):
                raise ValueError(f"truncated {label} string vector")
            values.append(payload[offset:offset + byte_length].decode(encoding))
            offset += byte_length
        if values != [None, "A\r", "B", None, "Y"]:
            raise ValueError(f"{label} PopLine results differ: {values}")
        results[label] = {
            "runtime_sha256": ORIGINAL_SHA256 if label == "original" else PLAYER_SHA256,
            "trace_sha256": sha256(trace_bytes),
            "save_sha256": sha256(save_bytes),
            "global_string_slots_1_to_5": values,
            "same_slot_no_lf_observer": "Y (slot 4 appended no text)",
        }
    result = {
        "manifest_sha256": sha256(manifest_bytes),
        "input_file_hashes": manifest["files"],
        "results": results,
        "scope": "3020 scalar PopLine of A\\r\\nB into slot 2 then remaining B into slot 3; same-slot no-LF pop is observed by appending slot 4 to Y in slot 5; range/negative/expansion variants remain unverified",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
