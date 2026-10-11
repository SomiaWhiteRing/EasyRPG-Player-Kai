"""Verify fixed original JS negative-ID accessors in map/common-event contexts."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
import pefile

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
FIXTURE = ROOT / "build/maniac-original-js-host-negative"
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
MANIFEST = EVIDENCE / "maniac-250921-js-host-negative-manifest.json"
TRACE = EVIDENCE / "maniac-250921-js-host-negative-trace.json"
SAVE = EVIDENCE / "maniac-250921-js-host-negative-save.lsd"
OUTPUT = EVIDENCE / "maniac-250921-js-host-negative-audit.json"
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
EXPECTED_RESULTS = [[0, False, ""],
                    [-1, 42, 42, True, True, "M", "M", 44, False, "MI"],
                    [0, False, ""],
                    [-1, 33, 33, True, True, "C", "C", 35, False, "CI"]]
CALLS = ((0x17F0C1, 0x17D7C0), (0x17F214, 0x8CAC0), (0x17F3CD, 0x8C070),
         (0x17F867, 0x8C9E0), (0x17FF71, 0x8C100), (0x17FC23, 0x8CCA0))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture() -> None:
    subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                    "--save-probe", "--save-variant", "js-host-negative",
                    "--output", str(FIXTURE)], check=True)
    shutil.copyfile(FIXTURE / "manifest.json", MANIFEST)
    subprocess.run([sys.executable, str(ROOT / "docs/probe-maniac-battle-noop.py"),
                    "--runtime", "original", "--save-variant", "js-host-negative",
                    "--fixture", str(FIXTURE), "--output", str(TRACE)], check=True)
    TRACE.with_suffix(".lsd").replace(SAVE)


def main() -> None:
    manifest = json.loads(MANIFEST.read_text())
    trace = json.loads(TRACE.read_text())
    if (sha256(ORIGINAL) != ORIGINAL_SHA or manifest["save_variant"] != "js-host-negative"
            or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA
            or trace["runtime_sha256"] != ORIGINAL_SHA or trace["runtime_arguments"]
            or trace["fixture_manifest_sha256"] != sha256(MANIFEST)
            or trace["save_file_sha256"] != sha256(SAVE)
            or trace["save_file_length"] != SAVE.stat().st_size or trace["save_slot"] != 1
            or trace["dispatch_events"]):
        raise ValueError("fixed original inputs or save changed")
    for name, digest in manifest["files"].items():
        if sha256(FIXTURE / name) != digest:
            raise ValueError(f"fixture input changed: {name}")
    helper = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    blob = SAVE.read_bytes()
    size, position = helper["ber"](blob, 0)
    if blob[position:position + size] != b"LcfSaveData":
        raise ValueError("wrong save header")
    root = {entry["id"]: entry for entry in helper["chunks"](blob, position + size, len(blob))}
    system = root[101]
    fields = {entry["id"]: entry for entry in helper["chunks"](
        blob, system["payload_offset"], system["payload_offset"] + system["length"])}
    def payload(field: int) -> bytes:
        entry = fields[field]
        return blob[entry["payload_offset"]:entry["payload_offset"] + entry["length"]]
    if payload(34) != struct.pack("<2i", -1, 99):
        raise ValueError("negative reference and post-JS continuation marker differ")
    strings = helper["decode_values"]({3: payload(36)}, "global strings")["field_3_observed_utf16_slots"]
    results = [json.loads(value) for value in strings]
    if results != EXPECTED_RESULTS:
        raise ValueError(f"direct/indirect/absent negative-ID results differ: {results}")
    extension = root[116]
    extra = blob[extension["payload_offset"]:extension["payload_offset"] + extension["length"]]
    children = {entry["id"]: entry for entry in helper["chunks"](extra, 0, len(extra))}
    common = helper["decode_common_self"](extra, children[1])
    maps = helper["decode_map_self_numbers"](extra, children[2])
    if common != [{"common_event_id": 3, "numbers_positive_slots": [35],
                   "field_3_observed_utf16_slots": ["CI"]}]:
        raise ValueError(f"common self bank changed: {common}")
    if maps != [{"map_id": 1, "events": [{"event_id": 1, "numbers_positive_slots": [44],
                                        "field_3_observed_utf16_slots": ["MI"]}],
                 "other_map_fields_hex": {}}]:
        raise ValueError(f"map self bank changed: {maps}")
    image = pefile.PE(str(ORIGINAL), fast_load=True)
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    native_calls = []
    for site, target in CALLS:
        instruction = next(decoder.disasm(image.get_data(site, 16), base + site))
        if (instruction.mnemonic != "call" or instruction.operands[0].type != X86_OP_IMM
                or instruction.operands[0].imm - base != target):
            raise ValueError(f"native accessor target changed at {site:#x}")
        native_calls.append({"site_rva": hex(site), "target_rva": hex(target),
                             "bytes": instruction.bytes.hex()})
    audit = {"runtime_sha256": ORIGINAL_SHA, "manifest_sha256": sha256(MANIFEST),
             "trace_sha256": sha256(TRACE), "save_sha256": sha256(SAVE),
             "global_numbers": [-1, 99], "json_results": results,
             "common_self": common, "map_self": maps, "native_accessor_calls": native_calls,
             "scope": "Fixed 250921 original numeric/switch/string getters and setters use direct negative ID -1 and an ID obtained via global getv(1)=-1 in map event 1 and common event 3. Both absent banks read zero/false/empty string; direct and resolved-ID writes read back and persist distinct map/common banks. The post-JS ControlVars marker 99 executes and saves. Indirection here means resolving the ID through a getter; array/proxy arguments, coercion, error order and Player JS remain separate."}
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: negative/indirect/absent IDs verified in map and common contexts")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    args = parser.parse_args()
    if args.capture:
        capture()
    main()
