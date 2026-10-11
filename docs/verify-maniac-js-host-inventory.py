"""Check the 250921 original JavaScript host own-property inventory."""

from __future__ import annotations

import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
OUTPUT = EVIDENCE / "maniac-250921-js-host-inventory-audit.json"
REFERENCE = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
MAP = "6ea51e732298039d1ea0da13beaf432050016b5b86b22996eb9b1d420fb47d30"
EXPECTED_GLOBAL_NAMES = (
    "AggregateError|Array|ArrayBuffer|Atomics|BigInt|BigInt64Array|BigUint64Array|Boolean|"
    "DataView|Date|Error|EvalError|Float32Array|Float64Array|Function|Infinity|Int16Array|"
    "Int32Array|Int8Array|InternalError|JSON|Map|Math|NaN|Number|Object|Promise|Proxy|"
    "RangeError|ReferenceError|Reflect|RegExp|Set|SharedArrayBuffer|String|Symbol|"
    "SyntaxError|TypeError|URIError|Uint16Array|Uint32Array|Uint8Array|Uint8ClampedArray|"
    "WeakMap|WeakSet|__date_clock|console|decodeURI|decodeURIComponent|encodeURI|"
    "encodeURIComponent|escape|eval|gets|gett|getv|globalThis|include|isFinite|isNaN|"
    "parseFloat|parseInt|rm2k3|sets|sett|setv|undefined|unescape"
)
EXPECTED = [EXPECTED_GLOBAL_NAMES, "getPicture", "input|log", "missing", "erase|file|id|move|scale|show"]


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    sha256, ber, chunks = (parser[name] for name in ("sha256", "ber", "chunks"))
    manifest_bytes = (EVIDENCE / "maniac-250921-js-host-inventory-manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    trace_bytes = (EVIDENCE / "maniac-250921-js-host-inventory-trace.json").read_bytes()
    trace = json.loads(trace_bytes)
    save_bytes = (EVIDENCE / "maniac-250921-js-host-inventory-save.lsd").read_bytes()
    if (manifest["save_variant"] != "js-host-inventory" or
            manifest["files"]["RPG_RT.exe"] != REFERENCE or
            manifest["files"]["Map0001.lmu"] != MAP or
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
    fields = {entry["id"]: entry for entry in chunks(
        save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])}

    def payload(field: int) -> bytes:
        entry = fields[field]
        return save_bytes[entry["payload_offset"]:entry["payload_offset"] + entry["length"]]

    if payload(33) != b"\x01" or payload(34) != struct.pack("<i", 99):
        raise ValueError("later event marker missing")
    raw = payload(36)
    offset = 0
    results = []
    while offset < len(raw):
        length, offset = ber(raw, offset)
        if length > 1000000 or offset + 2 * length > len(raw):
            raise ValueError("truncated or invalid original string vector")
        results.append(raw[offset:offset + 2 * length].decode("utf-16-le"))
        offset += 2 * length
    if results != EXPECTED:
        raise ValueError(f"original own-property inventory changed: {results}")
    audit = {
        "reference_sha256": REFERENCE,
        "manifest_sha256": sha256(manifest_bytes),
        "input_file_hashes": manifest["files"],
        "trace_sha256": sha256(trace_bytes),
        "save_sha256": sha256(save_bytes),
        "global_own_string_names": results[0].split("|"),
        "rm2k3_own_string_names": results[1].split("|"),
        "console_own_string_names": results[2].split("|"),
        "global_picture_constructor": results[3],
        "picture_instance_prototype_own_string_names": results[4].split("|"),
        "scope": "250921 original Object.getOwnPropertyNames on globalThis, rm2k3, console and the prototype of rm2k3.getPicture(1). It establishes own string-key names and later event continuation, not symbols, nested objects, getter behavior, callback semantics, or Player implementation.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: original host own-property inventory verified")


if __name__ == "__main__":
    main()
