"""Verify bounded 250921 original/Player evidence for standalone 3035."""

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
MANIFEST = EVIDENCE / "maniac-250921-drawshape-noop-manifest.json"
OUTPUT = EVIDENCE / "maniac-250921-3035-scope-audit.json"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
REPORT = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete/analysis-work/reports/03035-dac1676e1671ef584d649bff1125bad3e32d0a3a14c4e3f1dfc5a3b716434e6f.json"
ORIGINAL_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_SHA256 = "6b4d1f70ece4819871ed8138226ac2e4c252aea1226553040ddb9d058f4ebe94"
MAP_SHA256 = "cb03f10183daf5dc1666abda7e483f2859c201538a8163dde37e70cda1f8eed7"
ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921"]


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def check_save(label: str, manifest_bytes: bytes, ber, chunks) -> dict[str, object]:
    stem = "maniac-250921" if label == "original" else "maniac-player-250921"
    trace_bytes = (EVIDENCE / f"{stem}-drawshape-noop-trace.json").read_bytes()
    save_bytes = (EVIDENCE / f"{stem}-drawshape-noop-save.lsd").read_bytes()
    trace = json.loads(trace_bytes.decode("utf-8-sig"))
    if (trace["reference_sha256"] != ORIGINAL_SHA256 or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["save_file_sha256"] != sha256(save_bytes) or
            trace["save_file_length"] != len(save_bytes) or trace["save_slot"] != 1):
        raise ValueError(f"{label} 3035 trace/save mismatch")
    if label == "player" and (trace.get("player_sha256") != PLAYER_SHA256 or
                               trace.get("player_arguments") != ARGS):
        raise ValueError("Player 3035 invocation differs")
    length, position = ber(save_bytes, 0)
    if save_bytes[position:position + length] != b"LcfSaveData":
        raise ValueError(f"{label} save header changed")
    root = chunks(save_bytes, position + length, len(save_bytes))
    system = next(entry for entry in root if entry["id"] == 101)
    fields = {entry["id"]: entry for entry in chunks(
        save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])}
    values = fields[34]
    payload = save_bytes[values["payload_offset"]:values["payload_offset"] + values["length"]]
    if len(payload) < 4 or len(payload) % 4 or struct.unpack_from("<i", payload)[0] != 99:
        raise ValueError(f"{label} did not reach later variable marker")
    return {"trace_sha256": sha256(trace_bytes), "save_sha256": sha256(save_bytes),
            "save_observed_after_enter": trace["save_observed_after_enter"],
            "global_numeric_id_1": 99}


def main() -> None:
    helpers = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    if (manifest["save_variant"] != "drawshape-noop" or
            manifest["expected_save_slot"] != 1 or
            manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA256 or
            manifest["files"]["Map0001.lmu"] != MAP_SHA256 or
            sha256(RUNTIME.read_bytes()) != ORIGINAL_SHA256 or
            sha256(PLAYER.read_bytes()) != PLAYER_SHA256):
        raise ValueError("3035 runtime or fixture changed")
    fixture = ROOT / "build/maniac-original-drawshape-noop"
    for name, expected in manifest["files"].items():
        if sha256((fixture / name).read_bytes()) != expected:
            raise ValueError(f"3035 input changed: {name}")
    report_bytes = REPORT.read_bytes()
    report = json.loads(report_bytes)
    dispatches = [entry["dispatch"] for entry in report["inputs"]["deferred"]
                  if "dispatch" in entry]
    if (report["inputs"]["binaries"]["runtime"] != ORIGINAL_SHA256 or
            len(dispatches) != 1 or dispatches[0]["opcode"] != 3035 or
            dispatches[0]["branch_rva"] != "0x463e2"):
        raise ValueError("reviewed 3035 command-to-default-skip mapping changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    base = image.OPTIONAL_HEADER.ImageBase
    expected = {0x463E2: "mov eax, dword ptr [r13]", 0x463E6: "sub eax, 1",
                0x463E9: "js 0x140047d15", 0x463EF: "jmp 0x1400458e0"}
    observed = {}
    for rva, instruction in expected.items():
        decoded = list(decoder.disasm(image.get_data(rva, 16), base + rva, count=1))
        if len(decoded) != 1 or f"{decoded[0].mnemonic} {decoded[0].op_str}" != instruction:
            raise ValueError(f"3035 default-skip path changed at {rva:#x}")
        observed[f"0x{rva:x}"] = instruction
    results = {label: check_save(label, manifest_bytes, helpers["ber"], helpers["chunks"])
               for label in ("original", "player")}
    audit = {
        "runtime_sha256": ORIGINAL_SHA256,
        "player_sha256": PLAYER_SHA256,
        "manifest_sha256": sha256(manifest_bytes),
        "map_sha256": MAP_SHA256,
        "reviewed_dispatch_report_sha256": sha256(report_bytes),
        "dispatcher_default_skip_instructions": observed,
        "effective_branch_ids": ["3035:standalone-default-skip-no-payload"],
        "applicable_context_candidates": ["map autostart", "map parallel", "common event", "battle event"],
        "results": results,
        "scope": "Fixed 250921 original 3035 follows the dispatcher default skip path, re-fetching the next command without reading this command's Args/Text or drawing a shape. The original and Player map-autostart fixtures with nonempty Args/Text reach variable 1 marker 99 and Save01. Other event-context scheduling and Player yield parity remain open under W02-017.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: original and Player 3035 map event reaches marker and save")


if __name__ == "__main__":
    main()
