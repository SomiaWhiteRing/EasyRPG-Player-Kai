"""Check 250921 original JavaScript host accessors and subsequent save state."""

from __future__ import annotations

import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
OUTPUT = EVIDENCE / "maniac-250921-js-host-accessors-audit.json"
REFERENCE = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
EXPECTED_NUMBERS = [0, 1, 0, 0, 0, 0, 99, 0, 0, 42]
EXPECTED_STRINGS = ["true", "42", "true", "42", "true", "Host", None, None, None, "Host"]


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256, ber, chunks = (parser[name] for name in ("sha256", "ber", "chunks"))
    manifest_bytes = (EVIDENCE / "maniac-250921-js-host-accessors-manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    trace_bytes = (EVIDENCE / "maniac-250921-js-host-accessors-trace.json").read_bytes()
    trace = json.loads(trace_bytes)
    save_bytes = (EVIDENCE / "maniac-250921-js-host-accessors-save.lsd").read_bytes()
    if (manifest["save_variant"] != "js-host-accessors" or manifest["files"]["RPG_RT.exe"] != REFERENCE or
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
    fields = {entry["id"]: entry for entry in chunks(save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])}

    def payload(field: int) -> bytes:
        entry = fields[field]
        return save_bytes[entry["payload_offset"]:entry["payload_offset"] + entry["length"]]

    if payload(31) != b"\x0a" or payload(32) != b"\x01" + b"\0" * 8 + b"\x01":
        raise ValueError("global switch 10 was not set alongside existing switch 1")
    number_payload = payload(34)
    if payload(33) != b"\x0a" or len(number_payload) != 40:
        raise ValueError("wrong numeric vector length")
    numbers = list(struct.unpack("<10i", number_payload))
    if numbers != EXPECTED_NUMBERS:
        raise ValueError(f"numeric host writes or later marker differ: {numbers}")
    string_payload = payload(36)
    offset = 0
    values = []
    while offset < len(string_payload):
        length, offset = ber(string_payload, offset)
        if length >= (1 << 35) - 1000000:
            values.extend([None] * ((1 << 35) - length))
            continue
        byte_length = 2 * length
        if offset + byte_length > len(string_payload):
            raise ValueError("truncated original string vector")
        values.append(string_payload[offset:offset + byte_length].decode("utf-16-le"))
        offset += byte_length
    if values != EXPECTED_STRINGS:
        raise ValueError(f"string host writes or results differ: {values}")
    audit = {
        "reference_sha256": REFERENCE,
        "manifest_sha256": sha256(manifest_bytes),
        "input_file_hashes": manifest["files"],
        "trace_sha256": sha256(trace_bytes),
        "save_sha256": sha256(save_bytes),
        "global_switch_slots_1_to_10": [int(value != 0) for value in payload(32)],
        "global_numeric_slots_1_to_10": numbers,
        "global_string_slots_1_to_10": values,
        "scope": "250921 original global setv/sets/sett and getv/gets/gett at slot 10, array result distribution, and later 10220/3002 execution; negative/self IDs, coercion edges, exceptions, and Player JS implementation remain unverified",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: global host read/write results verified")


if __name__ == "__main__":
    main()
