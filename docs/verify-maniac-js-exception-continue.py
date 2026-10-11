"""Check an original 3030 exception boundary and later event commands."""

from __future__ import annotations

import argparse
import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
REFERENCES = {
    "250921": "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f",
    "241028": "49ba424f3ff1fa7d4fc1334eea272fde4e437d07a6901dc6b87efb95383ec44d",
}
MAP = "0f6e4cdeb9aa8941226fe1ab67d222d11479f3bfed599f4dac5fbd96cb4b8470"


def main() -> None:
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument("--reference-version", choices=REFERENCES, default="250921")
    version = arguments.parse_args().reference_version
    reference = REFERENCES[version]
    prefix = f"maniac-{version}-js-exception-continue"
    output = EVIDENCE / f"{prefix}-audit.json"
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256, ber, chunks = (parser[name] for name in ("sha256", "ber", "chunks"))
    manifest_bytes = (EVIDENCE / f"{prefix}-manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    trace_bytes = (EVIDENCE / f"{prefix}-trace.json").read_bytes()
    trace = json.loads(trace_bytes)
    save_bytes = (EVIDENCE / f"{prefix}-save.lsd").read_bytes()
    shared_inputs = ("RPG_RT.ldb", "RPG_RT.lmt", "Map0001.lmu", "ChipSet/probe.png")
    if version == "241028":
        newer = json.loads((EVIDENCE / "maniac-250921-js-exception-continue-manifest.json").read_bytes())
        if newer["files"]["RPG_RT.exe"] != REFERENCES["250921"] or any(
                manifest["files"][name] != newer["files"][name] for name in shared_inputs):
            raise ValueError("241028 and 250921 comparison inputs differ")
    if (manifest["save_variant"] != "js-exception-continue" or
            manifest["files"]["RPG_RT.exe"] != reference or
            manifest["files"]["Map0001.lmu"] != MAP or
            manifest["expected_global_slots_after_event"] != [0, 0, 42, 99] or
            trace["reference_sha256"] != reference or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["save_file_sha256"] != sha256(save_bytes) or
            trace["save_file_length"] != len(save_bytes) or
            trace["save_slot"] != 1):
        raise ValueError("original input or output binding failed")
    header_length, position = ber(save_bytes, 0)
    if save_bytes[position:position + header_length] != b"LcfSaveData":
        raise ValueError("invalid original save header")
    root = chunks(save_bytes, position + header_length, len(save_bytes))
    system = next(entry for entry in root if entry["id"] == 101)
    fields = {entry["id"]: entry for entry in chunks(
        save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])}

    def payload(field: int) -> bytes:
        entry = fields[field]
        return save_bytes[entry["payload_offset"]:entry["payload_offset"] + entry["length"]]

    if payload(33) != b"\x03" or len(payload(34)) != 12:
        raise ValueError("wrong numeric vector length")
    numbers = list(struct.unpack("<3i", payload(34)))
    if numbers != [0, 42, 99]:
        raise ValueError(f"exception or later event command results differ: {numbers}")
    string_payload = payload(36)
    count, offset = ber(string_payload, 0)
    if count != 9 or len(string_payload) != offset + 2 * count:
        raise ValueError("unexpected string result vector")
    string_result = string_payload[offset:].decode("utf-16-le")
    if string_result != "undefined":
        raise ValueError(f"exception string result differs: {string_result!r}")
    audit = {
        "reference_sha256": reference,
        "manifest_sha256": sha256(manifest_bytes),
        "input_file_hashes": manifest["files"],
        "shared_input_hashes": {name: manifest["files"][name] for name in shared_inputs},
        "trace_sha256": sha256(trace_bytes),
        "save_sha256": sha256(save_bytes),
        "global_numeric_slots_1_to_3": numbers,
        "global_string_slot_1": string_result,
        "scope": f"{version} original 3030 throw Error('boom') result destinations, subsequent 3030 expression 40+2, following 10220 marker, and 3002 save reached; other exception classes, modes, stack context, and Player JavaScript implementation remain unverified",
    }
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: original exception continuation verified")


if __name__ == "__main__":
    main()
