"""Check original and Player 3020 decimal/hexadecimal conversion saves."""

from __future__ import annotations

import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
OUTPUT = EVIDENCE / "maniac-250921-string-to-num-radix-audit.json"
ORIGINAL_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_SHA256 = "53b59d5b99ab92cf443fd5cd7f6cc924b3503095778e823e2e78e78aefa4a965"


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256, ber, chunks = (parser[name] for name in ("sha256", "ber", "chunks"))
    manifest_bytes = (EVIDENCE / "maniac-250921-string-to-num-radix-manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest["save_variant"] != "string-to-num-radix" or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA256:
        raise ValueError("wrong original fixture")
    results = {}
    for label in ("original", "player"):
        stem = f"maniac-250921-string-to-num-radix" if label == "original" else f"maniac-player-250921-string-to-num-radix"
        trace_bytes = (EVIDENCE / f"{stem}-trace.json").read_bytes()
        trace = json.loads(trace_bytes)
        save_bytes = (EVIDENCE / f"{stem}-save.lsd").read_bytes()
        if (trace["reference_sha256"] != ORIGINAL_SHA256 or
                trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
                trace["save_file_sha256"] != sha256(save_bytes) or
                trace["save_file_length"] != len(save_bytes) or trace["save_slot"] != 1):
            raise ValueError(f"{label} input/output binding failed")
        if label == "player" and (trace.get("player_sha256") != PLAYER_SHA256 or
                trace.get("player_arguments") != ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921"]):
            raise ValueError("Player executable or arguments changed")
        header_length, position = ber(save_bytes, 0)
        if save_bytes[position:position + header_length] != b"LcfSaveData":
            raise ValueError(f"invalid {label} save header")
        root = chunks(save_bytes, position + header_length, len(save_bytes))
        system = next(entry for entry in root if entry["id"] == 101)
        fields = chunks(save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])
        variables = next(entry for entry in fields if entry["id"] == 34)
        payload = save_bytes[variables["payload_offset"]:variables["payload_offset"] + variables["length"]]
        if len(payload) % 4:
            raise ValueError(f"unaligned {label} numeric vector")
        values = list(struct.unpack("<" + "i" * (len(payload) // 4), payload))
        if values[:4] != [8, 0, 16, 99]:
            raise ValueError(f"{label} radix results differ: {values}")
        results[label] = {
            "runtime_sha256": ORIGINAL_SHA256 if label == "original" else PLAYER_SHA256,
            "trace_sha256": sha256(trace_bytes),
            "save_sha256": sha256(save_bytes),
            "saved_global_variables_1_to_4": values[:4],
        }
    result = {
        "manifest_sha256": sha256(manifest_bytes),
        "input_file_hashes": manifest["files"],
        "results": results,
        "scope": "3020 assign '08'/'0x10', decimal ToNum for both and explicit hex ToNum for '0x10'; later marker 99 proves continuation; other numeric formats remain unverified",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
