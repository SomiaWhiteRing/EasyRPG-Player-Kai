"""Verify own string/symbol keys on the bounded 250921 JavaScript host surface."""

from __future__ import annotations

import hashlib
import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-js-host-symbols-manifest.json"
TRACE = EVIDENCE / "maniac-250921-js-host-symbols-trace.json"
SAVE = EVIDENCE / "maniac-250921-js-host-symbols-save.lsd"
OUTPUT = EVIDENCE / "maniac-250921-js-host-symbols-audit.json"
RUNTIME_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
MAP_SHA256 = "22900da4bf88a1f3eb46ba64d2fad5227e45369ebb26cc231f9d5c9a88007d3c"
FUNCTIONS = ("getPicture", "consoleLog", "consoleInput", "include", "getv", "gets",
             "gett", "setv", "sets", "sett")


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
    if (manifest["save_variant"] != "js-host-symbols" or manifest["expected_save_slot"] != 1 or
            manifest["files"]["RPG_RT.exe"] != RUNTIME_SHA256 or
            manifest["files"]["Map0001.lmu"] != MAP_SHA256 or
            trace["reference_sha256"] != RUNTIME_SHA256 or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["save_file_sha256"] != sha256(save_bytes) or
            trace["save_file_length"] != len(save_bytes) or trace["save_slot"] != 1):
        raise ValueError("original host symbol input or save binding changed")
    header_length, position = ber(save_bytes, 0)
    if save_bytes[position:position + header_length] != b"LcfSaveData":
        raise ValueError("not an original LSD save")
    root = chunks(save_bytes, position + header_length, len(save_bytes))
    system = next(entry for entry in root if entry["id"] == 101)
    fields = {entry["id"]: entry for entry in chunks(
        save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])}

    def payload(field: int) -> bytes:
        entry = fields[field]
        return save_bytes[entry["payload_offset"]:entry["payload_offset"] + entry["length"]]

    if payload(33) != b"\x01" or payload(34) != struct.pack("<i", 99):
        raise ValueError("later event marker did not execute")
    raw = payload(36)
    length, offset = ber(raw, 0)
    if length > 10000 or offset + 2 * length != len(raw):
        raise ValueError("unexpected saved string vector shape")
    rows = json.loads(raw[offset:].decode("utf-16-le"))
    if len(rows) != 15 or any(not isinstance(row, list) or len(row) != 2 for row in rows):
        raise ValueError("host key inventory shape changed")
    keys = {name: values for name, values in rows}
    if len(keys) != len(rows) or list(keys) != ["global", "rm2k3", "console", "picture",
                                       "pictureProto", *FUNCTIONS]:
        raise ValueError("host root and reachable object list changed")
    previous = json.loads((EVIDENCE / "maniac-250921-js-host-inventory-audit.json").read_text(encoding="utf-8"))
    expected_global = [f"string:{name}" for name in previous["global_own_string_names"]]
    if keys["global"] != sorted(expected_global):
        raise ValueError("global own keys changed or symbol key found")
    if (keys["rm2k3"] != ["string:getPicture"] or
            keys["console"] != ["string:input", "string:log"] or
            keys["picture"] != [] or
            keys["pictureProto"] != [f"string:{name}" for name in
                                     previous["picture_instance_prototype_own_string_names"]]):
        raise ValueError("nested host object keys changed")
    if any(keys[name] != ["string:length", "string:name"] for name in FUNCTIONS):
        raise ValueError("host function own keys changed")
    if any(key.startswith("symbol:") for values in keys.values() for key in values):
        raise ValueError("host object gained an own symbol key")
    audit = {
        "runtime_sha256": RUNTIME_SHA256,
        "manifest_sha256": sha256(manifest_bytes),
        "trace_sha256": sha256(trace_bytes),
        "save_sha256": sha256(save_bytes),
        "map_sha256": MAP_SHA256,
        "host_object_own_keys": keys,
        "scope": "250921 Windows original Reflect.ownKeys on the global object, rm2k3, console, one picture instance and its prototype, and all callable host names found in the direct registration inventory. No own Symbol keys are present in this bounded surface. Standard JS builtins, user-added properties and other runtime states are not enumerated; static registration reachability remains M0-028.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: 15 host-related objects and functions verified")


if __name__ == "__main__":
    main()
