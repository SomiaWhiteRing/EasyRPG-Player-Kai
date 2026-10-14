"""Verify 250921 opcode 40 refetch in map, common and battle contexts."""

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
CASES = {
    "opcode40-long-skip": {
        "values": [99, 99],
        "dispatch": {"opcode40_dispatch_count": 10050,
                     "owner_modes": {"0": 10050},
                     "first_cursor": 1, "last_cursor": 10050},
    },
    "opcode40-auto-common": {
        "values": [99, 77, 33],
        "dispatch": {"opcode40_dispatch_count": 10052,
                     "owner_modes": {"1": 10052},
                     "first_cursor": 1, "last_cursor": 10050},
    },
    "opcode40-battle": {
        "values": [99, 0, 0, 0, 0, 0, 0, 0, 0, 110],
        "dispatch": {"opcode40_dispatch_count": 10050,
                     "owner_modes": {"2": 10050},
                     "first_cursor": 1, "last_cursor": 10050},
    },
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
                arguments.append("--trace-opcode40")
            subprocess.run(arguments, check=True)


def verify() -> None:
    if sha256(ORIGINAL) != ORIGINAL_SHA:
        raise ValueError("fixed original binary changed")
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    saved_values = runpy.run_path(str(ROOT / "docs/verify-maniac-noop-long-skip.py"))["saved_values"]
    checked = []
    for variant, expected in CASES.items():
        fixture = ROOT / "build" / f"maniac-{variant}"
        manifest_path = EVIDENCE / f"maniac-250921-{variant}-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        if (manifest["save_variant"] != variant
                or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA
                or manifest["expected_save_slot"] != 1
                or manifest["expected_global_slots_after_event"] != expected["values"]):
            raise ValueError(f"wrong fixed fixture manifest: {variant}")
        for name, digest in manifest["files"].items():
            if sha256(fixture / name) != digest:
                raise ValueError(f"fixture input changed: {variant}/{name}")
        for runtime, executable, arguments in (("original", ORIGINAL, []),
                                               ("player", PLAYER, PLAYER_ARGS)):
            prefix = EVIDENCE / f"maniac-250921-{variant}-{runtime}-trace"
            trace_path = prefix.with_suffix(".json")
            save_path = prefix.with_suffix(".lsd")
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
            if numbers[:len(expected["values"])] != expected["values"] or any(numbers[len(expected["values"]):]):
                raise ValueError(f"saved markers changed: {variant}/{runtime}: {numbers}")
            if runtime == "original":
                if (not trace.get("opcode40_instrumented")
                        or trace["dispatch_events"] != [expected["dispatch"]]):
                    raise ValueError(f"original native opcode 40 dispatch changed: {variant}")
            elif trace.get("opcode40_instrumented") or trace["dispatch_events"]:
                raise ValueError(f"Player trace claims original native instrumentation: {variant}")
            checked.append({"variant": variant, "runtime": runtime,
                            "runtime_sha256": trace["runtime_sha256"],
                            "manifest_sha256": sha256(manifest_path),
                            "trace_sha256": sha256(trace_path),
                            "save_sha256": sha256(save_path), "numbers": numbers})
    audit = {"original_sha256": ORIGINAL_SHA, "player_sha256": sha256(PLAYER),
             "cases": checked,
             "scope": "250921 Windows opcode 40 in map-parallel, map-autostart parent branch scans, a called common event and a battle page. Native hook reads the active record to count opcode 40 only, with mode 0/1/2 dispatches before Save01. Encoded records preserve Args/Text/indent. Original and Player same-update marker saves agree; other versions and platforms have separate scopes."}
    output = EVIDENCE / "maniac-250921-opcode40-audit.json"
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: original/Player opcode 40 markers verified")


if __name__ == "__main__":
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument("--capture", action="store_true")
    args = arguments.parse_args()
    if args.capture:
        capture()
    verify()
