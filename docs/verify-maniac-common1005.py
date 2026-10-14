"""Verify valid opcode 1005 ID modes across fixed map/common/battle lists."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1",
               "--log-file", "noop-battle-player.log"]
CASES = {"common1005-auto": (1, 1), "common1005-parallel": (0, 1),
         "common1005-nested": (1, 2), "common1005-battle": (2, 2)}
RECORDS = [([1], 1), ([2, 0xF0, 777, -3], 2), ([30, 1], 1),
           ([-1, 1], 2), ([30, 0x12345001], 1)]
for mode in range(2, 16):
    RECORDS.extend(([operand, mode, 777, -3], selected)
                   for operand, selected in ((31, 1), (32, 2), (-2, 2), (-3, 2)))
RECORDS.append(([31, 0x12345002, 777, -3], 1))


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected_values(variant: str) -> list[int]:
    numbers = [0] * 161
    checksum = 0
    for index, (_, selected) in enumerate(RECORDS):
        checksum += selected
        numbers[99 + index] = checksum
    numbers[:4] = [106, 18, 44, 99]
    numbers[29:32] = [1, 30, -1]
    if variant == "common1005-battle":
        numbers[9] = 110
    return numbers


def capture() -> None:
    for variant in CASES:
        fixture = ROOT / "build" / f"maniac-{variant}"
        subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                        "--save-probe", "--save-variant", variant, "--output", str(fixture)], check=True)
        shutil.copyfile(fixture / "manifest.json", EVIDENCE / f"maniac-250921-{variant}-manifest.json")
        for runtime in ("original", "player"):
            arguments = [sys.executable, str(ROOT / "docs/probe-maniac-battle-noop.py"),
                         "--runtime", runtime, "--save-variant", variant,
                         "--fixture", str(fixture), "--output",
                         str(EVIDENCE / f"maniac-250921-{variant}-{runtime}-trace.json")]
            if runtime == "original":
                arguments.append("--trace-common1005")
            subprocess.run(arguments, check=True)


def verify() -> None:
    if sha256(ORIGINAL) != ORIGINAL_SHA:
        raise ValueError("fixed original changed")
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    saved_values = runpy.run_path(str(ROOT / "docs/verify-maniac-noop-long-skip.py"))["saved_values"]
    checked = []
    for variant, (mode, depth) in CASES.items():
        fixture = ROOT / "build" / f"maniac-{variant}"
        manifest_path = EVIDENCE / f"maniac-250921-{variant}-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        expected = expected_values(variant)
        if (manifest["save_variant"] != variant
                or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA
                or manifest["expected_save_slot"] != 1
                or manifest["expected_global_slots_after_event"] != expected):
            raise ValueError(f"wrong fixture manifest: {variant}")
        for name, digest in manifest["files"].items():
            if sha256(fixture / name) != digest:
                raise ValueError(f"fixture changed: {variant}/{name}")
        expected_dispatch = [dict(parameters=parameters, owner_mode=mode, depth_before=depth,
                                  depth_after=depth + 1, common_event_id=selected,
                                  frame_kind=0x25, cursor=0) for parameters, selected in RECORDS]
        if depth == 2:
            expected_dispatch.insert(0, dict(parameters=[3, 0xF0, 777, -3], owner_mode=mode,
                                            depth_before=1, depth_after=2, common_event_id=3,
                                            frame_kind=0x25, cursor=0))
        for runtime, executable, arguments in (("original", ORIGINAL, []),
                                               ("player", PLAYER, PLAYER_ARGS)):
            prefix = EVIDENCE / f"maniac-250921-{variant}-{runtime}-trace"
            trace_path, save_path = prefix.with_suffix(".json"), prefix.with_suffix(".lsd")
            trace = json.loads(trace_path.read_text())
            if (trace["reference_sha256"] != ORIGINAL_SHA
                    or trace["runtime_sha256"] != sha256(executable)
                    or trace["runtime_arguments"][:3] + trace["runtime_arguments"][-2:] != arguments
                    or trace["fixture_manifest_sha256"] != sha256(manifest_path)
                    or trace["save_file_sha256"] != sha256(save_path)
                    or trace["save_file_length"] != save_path.stat().st_size
                    or trace["save_slot"] != 1):
                raise ValueError(f"runtime/fixture/save provenance changed: {variant}/{runtime}")
            numbers = saved_values(save_path.read_bytes(), parser["ber"], parser["chunks"])
            if numbers[:len(expected)] != expected or any(numbers[len(expected):]):
                raise ValueError(f"per-call saved checksums differ: {variant}/{runtime}: {numbers}")
            if runtime == "original":
                if not trace.get("common1005_instrumented") or trace["dispatch_events"] != expected_dispatch:
                    raise ValueError(f"original parameters/frame selections differ: {variant}")
            elif trace.get("common1005_instrumented") or trace["dispatch_events"]:
                raise ValueError(f"Player claims original instrumentation: {variant}")
            checked.append(dict(variant=variant, runtime=runtime, runtime_sha256=trace["runtime_sha256"],
                                manifest_sha256=sha256(manifest_path), trace_sha256=sha256(trace_path),
                                save_sha256=sha256(save_path), checksum=numbers[0],
                                selected_counts=numbers[1:3], snapshots=numbers[99:161]))
    output = EVIDENCE / "maniac-250921-common1005-audit.json"
    audit = dict(original_sha256=ORIGINAL_SHA, player_sha256=sha256(PLAYER), cases=checked,
                 scope="250921 Windows opcode 1005 valid ID mode selection: literal one-field and flags-low-nibble-0 records, single global/self reads, all nibble values 2..15 for signed double reads, high-bit and extra Args/Text preservation, common ID and frame-kind assignment in map auto/parallel, nested common and battle contexts. Native hook binds actual parameter arrays to selected IDs; per-call cumulative saves prove both runtimes execute the selected lists. Wait/restart, full frame initialization, 3019 production and errors remain separate acceptance leaves.")
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: four contexts x two runtimes, 62 valid mode selections each")


if __name__ == "__main__":
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument("--capture", action="store_true")
    args = arguments.parse_args()
    if args.capture:
        capture()
    verify()
