"""Verify one identical ControlVars fixture on original 241028 and 250921."""

from __future__ import annotations

import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
AUDIT = EVIDENCE / "maniac-241028-controlvars-audit.json"
VERSIONS = {
    "241028": ("maniac-241028-controlvars", "49ba424f3ff1fa7d4fc1334eea272fde4e437d07a6901dc6b87efb95383ec44d"),
    "250921": ("maniac-250921-legacy-controlvars", "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"),
}


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256 = parser["sha256"]
    ber = parser["ber"]
    chunks = parser["chunks"]
    result = {}
    inputs = {}
    for version, (stem, executable_hash) in VERSIONS.items():
        manifest_bytes = (EVIDENCE / f"{stem}-manifest.json").read_bytes()
        manifest = json.loads(manifest_bytes)
        trace_bytes = (EVIDENCE / f"{stem}-trace.json").read_bytes()
        trace = json.loads(trace_bytes)
        save_bytes = (EVIDENCE / f"{stem}-save.lsd").read_bytes()
        if manifest["save_variant"] != "legacy-controlvars" or manifest["expected_save_slot"] != 1:
            raise ValueError(f"wrong fixture variant: {version}")
        if manifest["files"]["RPG_RT.exe"] != executable_hash or (
                trace["reference_sha256"] != executable_hash or
                trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
                trace["save_file_sha256"] != sha256(save_bytes) or
                trace["save_file_length"] != len(save_bytes) or
                trace["save_slot"] != 1):
            raise ValueError(f"original input/save binding failed: {version}")
        inputs[version] = {key: value for key, value in manifest["files"].items() if key != "RPG_RT.exe"}

        header_length, position = ber(save_bytes, 0)
        if save_bytes[position:position + header_length] != b"LcfSaveData":
            raise ValueError(f"invalid LSD header: {version}")
        root = chunks(save_bytes, position + header_length, len(save_bytes))
        if len({entry["id"] for entry in root}) != len(root):
            raise ValueError(f"duplicate LSD root chunk: {version}")
        system = next(entry for entry in root if entry["id"] == 101)
        fields = chunks(save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])
        variables = next(entry for entry in fields if entry["id"] == 34)
        payload = save_bytes[variables["payload_offset"]:variables["payload_offset"] + variables["length"]]
        if len(payload) != 8 or struct.unpack("<ii", payload) != (7, 7):
            raise ValueError(f"wrong saved variable values: {version}")
        if any(entry["id"] == 116 for entry in root):
            raise ValueError(f"unexpected self bank block: {version}")
        result[version] = {
            "reference_sha256": executable_hash,
            "manifest_sha256": sha256(manifest_bytes),
            "trace_sha256": sha256(trace_bytes),
            "save_sha256": sha256(save_bytes),
            "save_length": len(save_bytes),
            "variable_ids_1_and_2": [7, 7],
        }
    if inputs["241028"] != inputs["250921"]:
        raise ValueError("the two original runtimes did not execute identical project inputs")
    output = {
        "input_file_hashes": inputs["241028"],
        "versions": result,
        "scope": "Identical ControlVars literal write, variable copy, and 3002 save produce variable IDs 1 and 2 as 7 in both original runtimes",
    }
    AUDIT.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(output, indent=2))


if __name__ == "__main__":
    main()
