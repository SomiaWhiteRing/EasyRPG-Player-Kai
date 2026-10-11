"""Verify the original 241028/250921 ControlVars negative-self divergence."""

from __future__ import annotations

import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
OUTPUT = EVIDENCE / "maniac-241028-negative-self-audit.json"
VERSIONS = {
    "241028": "49ba424f3ff1fa7d4fc1334eea272fde4e437d07a6901dc6b87efb95383ec44d",
    "250921": "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f",
}


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256 = parser["sha256"]
    ber = parser["ber"]
    chunks = parser["chunks"]
    decode_map_self_numbers = parser["decode_map_self_numbers"]
    hashes = {}
    results = {}
    for version, exe_hash in VERSIONS.items():
        stem = f"maniac-{version}-negative-self"
        manifest_bytes = (EVIDENCE / f"{stem}-manifest.json").read_bytes()
        manifest = json.loads(manifest_bytes)
        trace_bytes = (EVIDENCE / f"{stem}-trace.json").read_bytes()
        trace = json.loads(trace_bytes)
        save_bytes = (EVIDENCE / f"{stem}-save.lsd").read_bytes()
        if manifest["save_variant"] != "legacy-negative-self" or manifest["expected_save_slot"] != 1:
            raise ValueError(f"wrong fixture variant: {version}")
        if manifest["files"]["RPG_RT.exe"] != exe_hash or (
                trace["reference_sha256"] != exe_hash or
                trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
                trace["save_file_sha256"] != sha256(save_bytes) or
                trace["save_file_length"] != len(save_bytes) or
                trace["save_slot"] != 1):
            raise ValueError(f"original input/save binding failed: {version}")
        hashes[version] = {key: value for key, value in manifest["files"].items() if key != "RPG_RT.exe"}

        header_length, position = ber(save_bytes, 0)
        if save_bytes[position:position + header_length] != b"LcfSaveData":
            raise ValueError(f"invalid LSD header: {version}")
        root = chunks(save_bytes, position + header_length, len(save_bytes))
        system = next(entry for entry in root if entry["id"] == 101)
        fields = chunks(save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])
        variables = next(entry for entry in fields if entry["id"] == 34)
        payload = save_bytes[variables["payload_offset"]:variables["payload_offset"] + variables["length"]]
        if len(payload) != 8:
            raise ValueError(f"unexpected global variable vector: {version}")
        values = list(struct.unpack("<ii", payload))
        if values != ([0, 99] if version == "241028" else [42, 99]):
            raise ValueError(f"unexpected global variable values: {version}: {values}")
        self_chunks = [entry for entry in root if entry["id"] == 116]
        if version == "241028":
            if self_chunks:
                raise ValueError("241028 unexpectedly saved a self bank extension")
            bank = []
        else:
            if len(self_chunks) != 1:
                raise ValueError("250921 self bank extension missing")
            extension = self_chunks[0]
            children = chunks(save_bytes, extension["payload_offset"],
                              extension["payload_offset"] + extension["length"])
            if [(entry["id"], entry["length"]) for entry in children] != [(2, 14)]:
                raise ValueError("unexpected 250921 self bank extension shape")
            bank = decode_map_self_numbers(save_bytes, children[0])
            expected_bank = [{"map_id": 1, "events": [{"event_id": 1,
                              "numbers_positive_slots": [42]}], "other_map_fields_hex": {}}]
            if bank != expected_bank:
                raise ValueError(f"unexpected 250921 self bank: {bank}")
        results[version] = {
            "reference_sha256": exe_hash,
            "manifest_sha256": sha256(manifest_bytes),
            "trace_sha256": sha256(trace_bytes),
            "save_sha256": sha256(save_bytes),
            "saved_global_variables_1_and_2": values,
            "saved_map_self_banks": bank,
        }
    if hashes["241028"] != hashes["250921"]:
        raise ValueError("the two versions used different project inputs")
    result = {
        "input_file_hashes": hashes["241028"],
        "versions": results,
        "scope": "Same ControlVars negative-self write/read and later marker 99: 241028 saves V1=0 without 0x74; 250921 saves V1=42 and map self=42",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
