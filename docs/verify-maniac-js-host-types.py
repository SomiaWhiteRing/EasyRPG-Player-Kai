"""Check a fixed 250921 original runtime probe of JavaScript host scopes."""

from __future__ import annotations

import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
OUTPUT = EVIDENCE / "maniac-250921-js-host-types-audit.json"
REFERENCE = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
EXPECTED = [
    "object", "object", "undefined", "function", "undefined", "undefined",
    "undefined", "undefined", "function", "function", "function", "function",
    "function", "function", "function", "undefined", "undefined", "function",
]


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256, ber, chunks = (parser[name] for name in ("sha256", "ber", "chunks"))
    manifest_bytes = (EVIDENCE / "maniac-250921-js-host-types-manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    trace_bytes = (EVIDENCE / "maniac-250921-js-host-types-trace.json").read_bytes()
    trace = json.loads(trace_bytes)
    save_bytes = (EVIDENCE / "maniac-250921-js-host-types-save.lsd").read_bytes()
    if (manifest["save_variant"] != "js-host-types" or manifest["files"]["RPG_RT.exe"] != REFERENCE or
            trace["reference_sha256"] != REFERENCE or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["save_file_sha256"] != sha256(save_bytes) or
            trace["save_file_length"] != len(save_bytes) or trace["save_slot"] != 1):
        raise ValueError("original input or output binding failed")
    header_length, position = ber(save_bytes, 0)
    if save_bytes[position:position + header_length] != b"LcfSaveData":
        raise ValueError("invalid original save header")
    root = chunks(save_bytes, position + header_length, len(save_bytes))
    system = next(entry for entry in root if entry["id"] == 101)
    fields = chunks(save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])
    variables = next(entry for entry in fields if entry["id"] == 34)
    number_payload = save_bytes[variables["payload_offset"]:variables["payload_offset"] + variables["length"]]
    if len(number_payload) != 4 or struct.unpack("<i", number_payload)[0] != 99:
        raise ValueError("later marker missing")
    strings = next(entry for entry in fields if entry["id"] == 36)
    payload = save_bytes[strings["payload_offset"]:strings["payload_offset"] + strings["length"]]
    offset = 0
    values = []
    while offset < len(payload):
        length, offset = ber(payload, offset)
        if length > 1000000 or offset + 2 * length > len(payload):
            raise ValueError("truncated or invalid string vector")
        values.append(payload[offset:offset + 2 * length].decode("utf-16-le"))
        offset += 2 * length
    if values != EXPECTED:
        raise ValueError(f"JavaScript host scope changed: {values}")
    audit = {
        "reference_sha256": REFERENCE,
        "manifest_sha256": sha256(manifest_bytes),
        "input_file_hashes": manifest["files"],
        "trace_sha256": sha256(trace_bytes),
        "save_sha256": sha256(save_bytes),
        "script_1_typeof_results": values[:9],
        "script_2_typeof_results": values[9:],
        "scope": "250921 original runtime typeof only. It proves observed global/host property presence and later command continuation, not callback behavior or Player compatibility.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(values)} original typeof values")


if __name__ == "__main__":
    main()
