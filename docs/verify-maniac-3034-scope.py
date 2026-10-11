"""Verify bounded original and Player map evidence for standalone 3034."""

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
MANIFEST = EVIDENCE / "maniac-250921-reserved-noop-manifest.json"
OUTPUT = EVIDENCE / "maniac-250921-3034-scope-audit.json"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
REPORT = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete/analysis-work/reports/03034-3a1d0a31805487fe42334d3815ede702c17e0eac341bd232a7d5ea78abaeefe4.json"
ORIGINAL_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_SHA256 = "6b4d1f70ece4819871ed8138226ac2e4c252aea1226553040ddb9d058f4ebe94"
MAP_SHA256 = "85fcb6fb45a191b7e65cc1eeb89a42242f7814d4bd8ed3b00122f75ba75b96e5"
ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921"]


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def check_save(label: str, manifest_bytes: bytes, ber, chunks) -> dict[str, object]:
    stem = "maniac-250921" if label == "original" else "maniac-player-250921"
    trace_bytes = (EVIDENCE / f"{stem}-reserved-noop-trace.json").read_bytes()
    save_bytes = (EVIDENCE / f"{stem}-reserved-noop-save.lsd").read_bytes()
    trace = json.loads(trace_bytes.decode("utf-8-sig"))
    if (trace["reference_sha256"] != ORIGINAL_SHA256 or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["save_file_sha256"] != sha256(save_bytes) or
            trace["save_file_length"] != len(save_bytes) or
            trace["save_slot"] != 1):
        raise ValueError(f"{label} trace does not bind to frozen input/output")
    if label == "player" and (trace.get("player_sha256") != PLAYER_SHA256 or
                               trace.get("player_arguments") != ARGS):
        raise ValueError("Player runtime or command arguments changed")
    length, position = ber(save_bytes, 0)
    if save_bytes[position:position + length] != b"LcfSaveData":
        raise ValueError(f"{label} save header changed")
    root = chunks(save_bytes, position + length, len(save_bytes))
    system = next(entry for entry in root if entry["id"] == 101)
    fields = {entry["id"]: entry for entry in chunks(
        save_bytes, system["payload_offset"], system["payload_offset"] + system["length"])}
    count = fields[33]
    values = fields[34]
    count_payload = save_bytes[count["payload_offset"]:count["payload_offset"] + count["length"]]
    number_payload = save_bytes[values["payload_offset"]:values["payload_offset"] + values["length"]]
    if not count_payload or count_payload[0] < 1 or len(number_payload) < 4 or len(number_payload) % 4:
        raise ValueError(f"{label} numeric save field is malformed")
    marker = struct.unpack_from("<i", number_payload)[0]
    if marker != 99:
        raise ValueError(f"{label} 3034 did not reach later marker and save")
    return {"trace_sha256": sha256(trace_bytes), "save_sha256": sha256(save_bytes),
            "save_observed_after_enter": trace["save_observed_after_enter"],
            "global_numeric_id_1": marker}


def main() -> None:
    helpers = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    if (manifest["save_variant"] != "reserved-noop" or
            manifest["expected_save_slot"] != 1 or
            manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA256 or
            manifest["files"]["Map0001.lmu"] != MAP_SHA256 or
            sha256(RUNTIME.read_bytes()) != ORIGINAL_SHA256 or
            sha256(PLAYER.read_bytes()) != PLAYER_SHA256):
        raise ValueError("3034 runtime or fixture changed")
    image = pefile.PE(str(RUNTIME))
    report_bytes = REPORT.read_bytes()
    report = json.loads(report_bytes)
    dispatches = [entry["dispatch"] for entry in report["inputs"]["deferred"]
                  if "dispatch" in entry]
    if (report["inputs"]["binaries"]["runtime"] != ORIGINAL_SHA256 or
            len(dispatches) != 1 or dispatches[0]["opcode"] != 3034 or
            dispatches[0]["branch_rva"] != "0x47d01" or
            dispatches[0]["stop_rva"] != "0x46ef3"):
        raise ValueError("reviewed 3034 command-to-dispatch mapping changed")
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    base = image.OPTIONAL_HEADER.ImageBase
    expected = {0x47D01: "mov al, 1", 0x47D03: "jmp 0x140046ef3"}
    observed = {}
    for rva, instruction in expected.items():
        decoded = list(decoder.disasm(image.get_data(rva, 16), base + rva, count=1))
        if len(decoded) != 1 or f"{decoded[0].mnemonic} {decoded[0].op_str}" != instruction:
            raise ValueError(f"3034 success exit changed at {rva:#x}")
        observed[f"0x{rva:x}"] = instruction
    results = {label: check_save(label, manifest_bytes, helpers["ber"], helpers["chunks"])
               for label in ("original", "player")}
    audit = {
        "runtime_sha256": ORIGINAL_SHA256,
        "player_sha256": PLAYER_SHA256,
        "manifest_sha256": sha256(manifest_bytes),
        "map_sha256": MAP_SHA256,
        "reviewed_dispatch_report_sha256": sha256(report_bytes),
        "dispatcher_success_instructions": observed,
        "effective_branch_ids": ["3034:standalone-explicit-success-no-payload"],
        "applicable_context_candidates": ["map autostart", "map parallel", "common event", "battle event"],
        "results": results,
        "scope": "250921 Windows original and Player map-autostart events both execute standalone 3034 with nonempty Args/Text, then reach variable 1 marker 99 and Save01. Reviewed dispatcher maps 3034 to an immediate success exit with no command payload read. Yield timing and parallel/common/battle scheduling remain separate Player feature work; this audit does not prove those contexts.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: original and Player map-autostart 3034 reach marker and save")


if __name__ == "__main__":
    main()
