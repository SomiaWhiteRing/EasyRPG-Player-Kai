"""Verify the bounded original 3024 independent-dispatch observation."""

from __future__ import annotations

import hashlib
import json
import runpy
import struct
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-reserved2-noop-manifest.json"
TRACE = EVIDENCE / "maniac-250921-reserved2-noop-trace.json"
SAVE = EVIDENCE / "maniac-250921-reserved2-noop-save.lsd"
OUTPUT = EVIDENCE / "maniac-250921-3024-scope-audit.json"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
RUNTIME_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
MAP_SHA256 = "fa4b4619b0243f6947abe1c00a98ccd1c4ecdeab978bdda7342a462dda40871c"


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
    if (manifest["save_variant"] != "reserved2-noop" or manifest["expected_save_slot"] != 1 or
            manifest["files"]["RPG_RT.exe"] != RUNTIME_SHA256 or
            manifest["files"]["Map0001.lmu"] != MAP_SHA256 or
            trace["reference_sha256"] != RUNTIME_SHA256 or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["save_file_sha256"] != sha256(save_bytes) or
            trace["save_file_length"] != len(save_bytes) or trace["save_slot"] != 1 or
            sha256(RUNTIME.read_bytes()) != RUNTIME_SHA256):
        raise ValueError("3024 scope fixture, runtime or original save changed")
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
        raise ValueError("3024 did not advance to the ControlVars marker and save")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    base = image.OPTIONAL_HEADER.ImageBase
    expected = {0x463E2: "mov eax, dword ptr [r13]", 0x47D01: "mov al, 1",
                0x47D03: "jmp 0x140046ef3"}
    observed = {}
    for rva, instruction in expected.items():
        decoded = list(decoder.disasm(image.get_data(rva, 16), base + rva, count=1))
        if len(decoded) != 1:
            raise ValueError(f"missing 3024 dispatcher instruction {rva:#x}")
        actual = f"{decoded[0].mnemonic} {decoded[0].op_str}"
        if actual != instruction:
            raise ValueError(f"3024 dispatcher instruction changed at {rva:#x}: {actual}")
        observed[f"0x{rva:x}"] = actual
    audit = {
        "runtime_sha256": RUNTIME_SHA256,
        "manifest_sha256": sha256(manifest_bytes),
        "trace_sha256": sha256(trace_bytes),
        "save_sha256": sha256(save_bytes),
        "map_sha256": MAP_SHA256,
        "original_marker_global_numeric_id_1": 99,
        "original_save_observed_after_enter": trace["save_observed_after_enter"],
        "dispatcher_instructions": observed,
        "effective_branch_ids": ["3024:standalone-default-no-payload"],
        "context_observed": "map autostart event",
        "remaining_contexts": ["map parallel event", "common event", "battle event"],
        "scope": "250921 Windows original 3024 with three nonempty arguments and nonempty Text in one map autostart event advances to ControlVars 99 and 3002 save. Reviewed static command contract identifies independent 3024 as a default path with no parameter/Text consumer. Scheduling and Player parity in other interpreter contexts remain separate acceptance work.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: original standalone 3024 advances to marker and save")


if __name__ == "__main__":
    main()
