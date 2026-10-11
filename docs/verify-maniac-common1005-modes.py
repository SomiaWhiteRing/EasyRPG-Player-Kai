"""Differentially verify non-inline 3019 producers of opcode 1005."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
BUILD = ROOT / "build"
RUNNER = BUILD / "windows-x64-vs2022-release-local/Release/test_runner.exe"
PLAYER = RUNNER.with_name("Player.exe")
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921",
               "--log-file", "noop-battle-player.log"]
CONTEXTS = {"auto": 1, "parallel": 0, "nested": 1, "battle": 2}
GENERATED = [
    ([1], 1, "RAW_TEXT"),
    ([2], 2, "DYNAMIC_TEXT"),
    ([1], 1, "DYNAMIC_TEXT"),
    ([2], 2, "SELF_TEXT"),
    ([1, 0, 0], 1, "DYNAMIC_TEXT"),
    ([2, 0], 2, "EXPR_VAR_TEXT"),
    ([1, 0, 0], 1, "EXPR_ASSIGN_TEXT"),
]
EXPECTED = [0] * 251
EXPECTED[0:5] = [4, 3, 0, 99, 0]
EXPECTED[99:106] = [1, 1, 2, 2, 3, 3, 4]
EXPECTED[109:116] = [0, 1, 1, 2, 2, 3, 3]
for slot, value in {200: 1, 210: 200, 211: 210, 212: 1, 213: 212,
                    220: 1005, 221: 220, 230: 10, 240: 2}.items():
    EXPECTED[slot - 1] = value
EXPECTED[250] = 1


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture() -> None:
    generator = ROOT / "docs/generate-maniac-original-fixture.py"
    probe = ROOT / "docs/probe-maniac-battle-noop.py"
    for context in CONTEXTS:
        source = BUILD / f"maniac-common1005-{context}"
        variant = f"common1005-modes-{context}"
        fixture = BUILD / f"maniac-{variant}"
        subprocess.run([sys.executable, str(generator), "--save-probe", "--save-variant",
                        f"common1005-{context}", "--output", str(source)], check=True)
        if not fixture.resolve().is_relative_to(BUILD.resolve()):
            raise ValueError(f"fixture path escaped build: {fixture}")
        if fixture.exists():
            shutil.rmtree(fixture)
        shutil.copytree(source, fixture)
        env = dict(os.environ, MANIAC_COMMON1005_MODES_DIR=str(fixture),
                   MANIAC_COMMON1005_MODES_CONTEXT=context)
        subprocess.run([str(RUNNER), "--no-colors",
                        "--test-case=Maniac 3019 non-inline 1005 fixture serialization"],
                       cwd=ROOT, env=env, check=True)
        manifest = json.loads((fixture / "manifest.json").read_text())
        expected = EXPECTED.copy()
        if context == "nested":
            expected[4] = 55
        if context == "battle":
            expected[9] = 110
        manifest.update(save_variant=variant, acceptance_id="W02-030",
                        probe="3019 opcode/argument/count/Text variable, indirect, self-array and expression sources generate seven 1005 calls",
                        verification_status="fixed original/Player non-inline generated 1005 differential",
                        expected_global_slots_after_event=expected)
        for name in manifest["files"]:
            manifest["files"][name] = sha(fixture / name)
        frozen = EVIDENCE / f"maniac-250921-{variant}-manifest.json"
        frozen.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        shutil.copyfile(frozen, fixture / "manifest.json")
        for runtime in ("original", "player"):
            command = [sys.executable, str(probe), "--runtime", runtime,
                       "--save-variant", variant, "--fixture", str(fixture),
                       "--output", str(EVIDENCE / f"maniac-250921-{variant}-{runtime}-trace.json"),
                       "--timeout", "45"]
            if runtime == "original":
                command.append("--trace-common1005-text")
            attempts = []
            attempts_path = EVIDENCE / f"maniac-250921-{variant}-{runtime}-capture-attempts.json"
            for attempt in range(1, 4):
                result = subprocess.run(command, capture_output=True, text=True)
                attempts.append(dict(attempt=attempt, exit_code=result.returncode,
                                     stdout_tail=result.stdout[-3000:], stderr_tail=result.stderr[-3000:]))
                attempts_path.write_text(json.dumps(dict(command=command[1:], attempts=attempts), indent=2) + "\n",
                                         encoding="utf-8")
                if result.returncode == 0:
                    print(f"{variant}/{runtime}: captured on attempt {attempt}", flush=True)
                    break
                if "did not create a stable save" not in result.stderr or attempt == 3:
                    raise RuntimeError(f"{variant}/{runtime} capture failed; see {attempts_path}")
            else:
                raise RuntimeError(f"{variant}/{runtime} capture exhausted retries")


def verify() -> None:
    save_values = runpy.run_path(str(ROOT / "docs/verify-maniac-common1005-frame.py"))["save_values"]
    checks = []
    for context, mode in CONTEXTS.items():
        variant = f"common1005-modes-{context}"
        fixture = BUILD / f"maniac-{variant}"
        frozen = EVIDENCE / f"maniac-250921-{variant}-manifest.json"
        manifest = json.loads(frozen.read_text())
        wanted = EXPECTED.copy()
        if context == "nested":
            wanted[4] = 55
        if context == "battle":
            wanted[9] = 110
        if (manifest["save_variant"] != variant or manifest["acceptance_id"] != "W02-030"
                or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA
                or manifest["expected_global_slots_after_event"] != wanted):
            raise ValueError(f"wrong producer fixture: {context}")
        for name, digest in manifest["files"].items():
            if sha(fixture / name) != digest:
                raise ValueError(f"producer fixture changed: {context}/{name}")
        native_expected = [([3], 3, "")]
        if context == "nested":
            native_expected.insert(0, ([4], 4, ""))
        native_expected += GENERATED
        for runtime in ("original", "player"):
            attempts_path = EVIDENCE / f"maniac-250921-{variant}-{runtime}-capture-attempts.json"
            attempts = json.loads(attempts_path.read_text())["attempts"]
            if not attempts or attempts[-1]["exit_code"] != 0:
                raise ValueError(f"producer capture lacks successful final attempt: {context}/{runtime}")
            prefix = EVIDENCE / f"maniac-250921-{variant}-{runtime}-trace"
            trace_path, save_path = prefix.with_suffix(".json"), prefix.with_suffix(".lsd")
            trace = json.loads(trace_path.read_text())
            if (trace["runtime_sha256"] != (ORIGINAL_SHA if runtime == "original" else sha(PLAYER))
                    or trace["runtime_arguments"] != ([] if runtime == "original" else PLAYER_ARGS)
                    or trace["fixture_manifest_sha256"] != sha(frozen)
                    or trace["save_file_sha256"] != sha(save_path)
                    or trace["save_file_length"] != save_path.stat().st_size):
                raise ValueError(f"producer provenance changed: {context}/{runtime}")
            numbers, common, maps = save_values(save_path)
            if (len(numbers) < 251 or numbers[:len(wanted)] != wanted[:len(numbers)]
                    or any(numbers[len(wanted):])):
                changed = [(index + 1, actual, expected) for index, (actual, expected)
                           in enumerate(zip(numbers, wanted)) if actual != expected][:15]
                raise ValueError(f"producer continuation differs: {context}/{runtime}: {changed}")
            banks = {bank["common_event_id"]: bank for bank in common}
            if 3 not in banks or banks[3]["numbers_positive_slots"][199] != 2:
                raise ValueError(f"negative self array source differs: {context}/{runtime}")
            if runtime == "original":
                records = trace["dispatch_events"]
                if not trace.get("common1005_text_instrumented") or len(records) != len(native_expected):
                    raise ValueError(f"generated consumer not reached: {context}")
                for record, (params, selected, expected_text) in zip(records, native_expected):
                    if (record["parameters"] != params or record["common_event_id"] != selected
                            or record["text"] != expected_text or record["owner_mode"] != mode
                            or record["depth_after"] != record["depth_before"] + 1
                            or record["frame_kind"] != 0x25 or record["cursor"] != 0):
                        raise ValueError(f"generated 1005 differs: {context}: {record}")
            elif trace.get("common1005_text_instrumented") or trace["dispatch_events"]:
                raise ValueError(f"Player claims native hooks: {context}")
            checks.append(dict(context=context, runtime=runtime, trace_sha256=sha(trace_path),
                               save_sha256=sha(save_path), attempts_sha256=sha(attempts_path),
                               attempt_count=len(attempts), checkpoints=numbers[99:117],
                               common3_self_slot200=banks[3]["numbers_positive_slots"][199]))
    output = EVIDENCE / "maniac-250921-common1005-modes-audit.json"
    output.write_text(json.dumps(dict(original_sha256=ORIGINAL_SHA, player_sha256=sha(PLAYER),
        runner_sha256=sha(RUNNER), cases=checks,
        scope="W02-030: 250921 Windows 3019 non-inline opcode 1005 production. Global raw/variable/indirect and negative-self array sources, literal and variable-expression sources, opcode and count references, and literal/direct/indirect Text sources execute the real 1005 consumer in four map/common/battle contexts. Original native hooks check generated Params/Text/selected ID/frame; original/Player saves check every call's continuation."), indent=2) + "\n", encoding="utf-8")
    print(f"{output}: {len(checks)} non-inline producer outcomes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    args = parser.parse_args()
    if args.capture:
        capture()
    verify()
