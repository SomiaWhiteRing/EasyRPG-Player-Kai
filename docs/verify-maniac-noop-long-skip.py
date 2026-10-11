"""Verify the fixed 250921 parallel/common 3024/3034/3035 comparison."""

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
FIXTURE = ROOT / "build/maniac-250921-noop-long-skip"
MANIFEST = EVIDENCE / "maniac-250921-noop-long-skip-manifest.json"
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
ORIGINAL_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921"]
OUTPUT = EVIDENCE / "maniac-250921-noop-long-skip-audit.json"


def digest(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def saved_values(blob: bytes, ber, chunks) -> list[int]:
    header_length, position = ber(blob, 0)
    if blob[position:position + header_length] != b"LcfSaveData":
        raise ValueError("unexpected save header")
    root = chunks(blob, position + header_length, len(blob))
    system = next(part for part in root if part["id"] == 101)
    fields = {part["id"]: part for part in chunks(
        blob, system["payload_offset"], system["payload_offset"] + system["length"])}
    numbers = fields[34]
    raw = blob[numbers["payload_offset"]:numbers["payload_offset"] + numbers["length"]]
    if len(raw) % 4:
        raise ValueError("unaligned numeric variable vector")
    return [struct.unpack_from("<i", raw, index)[0] for index in range(0, len(raw), 4)]


def inspect(label: str, manifest_hash: str, expected: list[int], ber, chunks) -> dict[str, object]:
    trace_bytes = (EVIDENCE / f"maniac-250921-noop-long-skip-{label}-trace.json").read_bytes()
    save_bytes = (EVIDENCE / f"maniac-250921-noop-long-skip-{label}-save.lsd").read_bytes()
    trace = json.loads(trace_bytes.decode("utf-8-sig"))
    if (trace["reference_sha256"] != ORIGINAL_SHA256 or
            trace["fixture_manifest_sha256"] != manifest_hash or
            trace["save_file_sha256"] != digest(save_bytes) or
            trace["save_file_length"] != len(save_bytes) or
            trace["save_slot"] != 1 or
            trace["save_observed_after_enter"] != (1 if label == "original" else 0)):
        raise ValueError(f"{label} trace is not bound to the fixed input and save")
    if label == "player" and not trace.get("autostart_without_input"):
        raise ValueError("Player trace lacks autonomous fixture startup provenance")
    if label == "player" and (trace.get("player_sha256") != digest(PLAYER.read_bytes()) or
                              trace.get("player_arguments") != ARGS):
        raise ValueError("Player runtime or arguments changed")
    values = saved_values(save_bytes, ber, chunks)
    if values[:len(expected)] != expected:
        raise ValueError(f"{label} parallel/common command order differs: {values}")
    return {"trace_sha256": digest(trace_bytes), "save_sha256": digest(save_bytes),
            "global_numeric_ids_1_to_5": values[:len(expected)],
            "numeric_vector_length": len(values)}


def main() -> None:
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    if (manifest["save_variant"] != "noop-long-skip" or
            manifest["expected_save_slot"] != 1 or
            manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA256 or
            digest(ORIGINAL.read_bytes()) != ORIGINAL_SHA256):
        raise ValueError("original runtime or fixture manifest changed")
    for name, expected_hash in manifest["files"].items():
        if digest((FIXTURE / name).read_bytes()) != expected_hash:
            raise ValueError(f"fixture input changed: {name}")
    image = pefile.PE(str(ORIGINAL))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    expected_instructions = {
        0x463E2: "mov eax, dword ptr [r13]",
        0x463EF: "jmp 0x1400458e0",
        0x47D01: "mov al, 1",
        0x47D03: "jmp 0x140046ef3",
    }
    observed = {}
    for rva, expected_text in expected_instructions.items():
        instruction = next(decoder.disasm(image.get_data(rva, 16), image.OPTIONAL_HEADER.ImageBase + rva))
        actual = f"{instruction.mnemonic} {instruction.op_str}"
        if actual != expected_text:
            raise ValueError(f"dispatcher branch changed at {rva:#x}: {actual}")
        observed[f"0x{rva:x}"] = actual
    helpers = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    expected_values = manifest["expected_global_slots_after_event"][1:]
    results = {label: inspect(label, digest(manifest_bytes), expected_values,
                              helpers["ber"], helpers["chunks"])
               for label in ("original", "player")}
    audit = {
        "original_sha256": ORIGINAL_SHA256,
        "player_sha256": digest(PLAYER.read_bytes()),
        "manifest_sha256": digest(manifest_bytes),
        "dispatch_instructions": observed,
        "results": results,
        "scope": "Fixed 250921 original and Player run 10050 alternating standalone 3024/3035 records in map parallel event 1, followed by a called common event containing standalone 3024, 3034 and 3035 with nonempty Args/Text. Parallel event 2 snapshots variable 1 in the same update and saves variables 1..5. This demonstrates continuous-fetch budget parity and common-event continuation for these records. Battle events, short records and other releases remain separate work.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: original and Player variable order {expected_values}")


if __name__ == "__main__":
    main()
