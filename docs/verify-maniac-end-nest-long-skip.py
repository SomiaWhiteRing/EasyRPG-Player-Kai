"""Verify original/Player 250921 End Nest long-list refetch behavior."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
FIXTURE = ROOT / "build/maniac-end-nest-long-skip"
MANIFEST = EVIDENCE / "maniac-250921-end-nest-long-skip-manifest.json"
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921",
               "--log-file", "noop-battle-player.log"]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture_all() -> None:
    subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                    "--save-probe", "--save-variant", "end-nest-long-skip",
                    "--output", str(FIXTURE)], check=True)
    shutil.copyfile(FIXTURE / "manifest.json", MANIFEST)
    for runtime in ("original", "player"):
        arguments = [sys.executable, str(ROOT / "docs/probe-maniac-battle-noop.py"),
                     "--runtime", runtime, "--save-variant", "end-nest-long-skip",
                     "--fixture", str(FIXTURE), "--output",
                     str(EVIDENCE / f"maniac-250921-end-nest-long-skip-{runtime}-trace.json")]
        if runtime == "original":
            arguments.append("--trace-end-nest")
        subprocess.run(arguments, check=True)


def main() -> None:
    manifest = json.loads(MANIFEST.read_text())
    if (manifest["save_variant"] != "end-nest-long-skip"
            or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA
            or manifest["expected_save_slot"] != 1
            or manifest["expected_global_slots_after_event"] != [99, 99]
            or sha256(ORIGINAL) != ORIGINAL_SHA):
        raise ValueError("wrong fixed original End Nest fixture")
    for name, digest in manifest["files"].items():
        if sha256(FIXTURE / name) != digest:
            raise ValueError(f"fixture input changed: {name}")
    image = pefile.PE(str(ORIGINAL))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    for rva, expected in ((0x463E2, "mov eax, dword ptr [r13]"),
                          (0x463EF, "jmp 0x1400458e0")):
        instruction = next(decoder.disasm(image.get_data(rva, 16), image.OPTIONAL_HEADER.ImageBase + rva))
        if f"{instruction.mnemonic} {instruction.op_str}" != expected:
            raise ValueError(f"original End Nest dispatch changed at {rva:#x}")
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    values = runpy.run_path(str(ROOT / "docs/verify-maniac-noop-long-skip.py"))["saved_values"]
    cases = []
    for runtime, executable, arguments in (("original", ORIGINAL, []),
                                           ("player", PLAYER, PLAYER_ARGS)):
        prefix = EVIDENCE / f"maniac-250921-end-nest-long-skip-{runtime}-trace"
        trace_path = prefix.with_suffix(".json")
        save_path = prefix.with_suffix(".lsd")
        trace = json.loads(trace_path.read_text())
        if (trace["reference_sha256"] != ORIGINAL_SHA
                or trace["runtime_sha256"] != sha256(executable)
                or trace["runtime_arguments"] != arguments
                or trace["fixture_manifest_sha256"] != sha256(MANIFEST)
                or trace["save_file_sha256"] != sha256(save_path)
                or trace["save_file_length"] != save_path.stat().st_size
                or trace["save_slot"] != 1):
            raise ValueError(f"runtime/fixture/save provenance changed: {runtime}")
        numbers = values(save_path.read_bytes(), parser["ber"], parser["chunks"])
        if numbers[:2] != [99, 99] or any(numbers[2:]):
            raise ValueError(f"wrong same-update parallel snapshot: {runtime}: {numbers}")
        if runtime == "original":
            if (not trace.get("end_nest_instrumented") or len(trace["dispatch_events"]) != 1
                    or trace["dispatch_events"][0] != {"end_nest_dispatch_count": 10050,
                                                         "owner_modes": {"0": 10050},
                                                         "first_cursor": 1,
                                                         "last_cursor": 10050}):
                raise ValueError("original native End Nest count/cursor changed")
        elif trace.get("end_nest_instrumented") or trace["dispatch_events"]:
            raise ValueError("Player trace must not claim original native instrumentation")
        cases.append({"runtime": runtime, "runtime_sha256": trace["runtime_sha256"],
                      "trace_sha256": sha256(trace_path), "save_sha256": sha256(save_path),
                      "numbers": numbers})
    audit = {"original_sha256": ORIGINAL_SHA, "player_sha256": sha256(PLAYER),
             "manifest_sha256": sha256(MANIFEST), "cases": cases,
             "scope": "250921 Windows map-parallel long-list opcode 10 refetch only. Original native hook counts 10050 opcode-10 dispatches in owner mode 0 before the first save and observes cursor 1 through 10050. Both runtimes place marker 99 before a second parallel event's same-update snapshot and save [99,99]. Encoded opcode-10 records carry three retained Args and nonempty Text; the handler ignores them. Common-event, battle, branch-scan and other version/platform parity remain W02-021."}
    output = EVIDENCE / "maniac-250921-end-nest-long-skip-audit.json"
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: original/Player first-update markers [99,99]")


if __name__ == "__main__":
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument("--capture", action="store_true")
    args = arguments.parse_args()
    if args.capture:
        capture_all()
    main()
