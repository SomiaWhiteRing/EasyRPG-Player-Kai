"""Capture and verify one active 1005 wait across an actual process restart."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
VARIANT = "common1005-wait-auto"
CONTEXT = "auto"
RUNNER = ROOT / "build/windows-x64-vs2022-release-local/Release/test_runner.exe"
PLAYER = RUNNER.with_name("Player.exe")
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
CAPTURED_PLAYER_SHA = {
    "auto": "6dff2314eb92269b91326c598118edc3870196c453421d28e9e4a3307f67d516",
    "parallel": "6dff2314eb92269b91326c598118edc3870196c453421d28e9e4a3307f67d516",
    "nested": "6dff2314eb92269b91326c598118edc3870196c453421d28e9e4a3307f67d516",
    "call12330-auto": "15c0c511d88affc27b6a72047b586da43fc1fc23d02572fe892718617c5c8cc9",
    "call12330-parallel": "15c0c511d88affc27b6a72047b586da43fc1fc23d02572fe892718617c5c8cc9",
    "call12330-nested": "15c0c511d88affc27b6a72047b586da43fc1fc23d02572fe892718617c5c8cc9",
}
PARSER = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
SAVE_VALUES = runpy.run_path(str(ROOT / "docs/verify-maniac-common1005-frame.py"))["save_values"]


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture_stage_once(fixture: Path, runtime: str, stage: int) -> None:
    slot = stage
    target = fixture / f"Save{slot:02d}.lsd"
    target.unlink(missing_ok=True)
    executable = fixture / "RPG_RT.exe" if runtime == "original" else PLAYER
    arguments = ([] if runtime == "original" else
                 ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921"] if stage == 1 else
                 ["--load-game-id", "1", "--patch-maniac", "1", "--maniac-version", "250921"])
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(executable), *arguments], cwd=fixture,
                               startupinfo=startup, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    try:
        deadline = time.monotonic() + 45
        previous = None
        stable_since = time.monotonic()
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"{runtime} stage {stage} exited {process.returncode}")
            try:
                digest = sha(target) if target.exists() else None
            except PermissionError:
                digest = None
            if digest and digest == previous and time.monotonic() - stable_since >= 1:
                break
            if digest != previous:
                previous = digest
                stable_since = time.monotonic()
            time.sleep(0.1)
        else:
            raise RuntimeError(f"{runtime} stage {stage} produced no stable Save{slot:02d}")
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
    frozen = EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-slot{slot:02d}.lsd"
    shutil.copyfile(target, frozen)
    trace = EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-stage{stage}.json"
    trace.write_text(json.dumps({
        "runtime": runtime, "stage": stage, "runtime_sha256": sha(executable),
        "runtime_arguments": arguments, "fixture_manifest_sha256": sha(fixture / "manifest.json"),
        "input_save_sha256": (sha(EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-slot01.lsd")
                              if stage == 2 else None),
        "output_save_sha256": sha(frozen), "output_save_length": frozen.stat().st_size,
        "observation": "Fresh process for each stage; Save01 remains in fixture for stage 2; captured after one unchanged second, then process terminated.",
    }, indent=2) + "\n", encoding="utf-8")
    print(f"{trace}: Save{slot:02d} {frozen.stat().st_size} bytes")


def capture_stage(fixture: Path, runtime: str, stage: int) -> None:
    attempts = []
    path = EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-stage{stage}-attempts.json"
    for attempt in range(1, 4):
        try:
            capture_stage_once(fixture, runtime, stage)
            attempts.append({"attempt": attempt, "result": "captured"})
            break
        except RuntimeError as error:
            attempts.append({"attempt": attempt, "result": str(error)})
            if attempt == 3:
                raise
        finally:
            path.write_text(json.dumps(attempts, indent=2) + "\n", encoding="utf-8")


def capture() -> None:
    for runtime in ("original", "player"):
        fixture = ROOT / "build" / f"maniac-{VARIANT}-{runtime}"
        subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                        "--save-probe", "--save-variant", VARIANT, "--output", str(fixture)], check=True)
        frozen = EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-manifest.json"
        shutil.copyfile(fixture / "manifest.json", frozen)
        capture_stage(fixture, runtime, 1)
        capture_stage(fixture, runtime, 2)


def frame_state(path: Path, context: str = "auto") -> dict:
    blob = path.read_bytes()
    length, pos = PARSER["ber"](blob, 0)
    if blob[pos:pos + length] != b"LcfSaveData":
        raise ValueError("wrong save header")
    root = {part["id"]: part for part in PARSER["chunks"](blob, pos + length, len(blob))}
    event = root[113]
    event_fields = {part["id"]: part for part in PARSER["chunks"](
        blob, event["payload_offset"], event["payload_offset"] + event["length"])}
    if context == "parallel":
        map_info = root[111]
        map_fields = {part["id"]: part for part in PARSER["chunks"](
            blob, map_info["payload_offset"], map_info["payload_offset"] + map_info["length"])}
        entries = map_fields[11]
        payload = blob[entries["payload_offset"]:entries["payload_offset"] + entries["length"]]
        events = PARSER["decode_banks"](payload, "map events")
        event = next(entry for entry in events if entry["id"] == 3)
        state = event["fields"][108]
        event_fields = {part["id"]: part for part in PARSER["chunks"](state, 0, len(state))}
        blob = state
    stack_field = event_fields[1]
    raw = blob[stack_field["payload_offset"]:stack_field["payload_offset"] + stack_field["length"]]
    count, pos = PARSER["ber"](raw, 0)
    frames = []
    for _ in range(count):
        frame_id, pos = PARSER["ber"](raw, pos)
        fields = {}
        while raw[pos]:
            field_id, pos = PARSER["ber"](raw, pos)
            size, pos = PARSER["ber"](raw, pos)
            fields[field_id] = raw[pos:pos + size]
            pos += size
        pos += 1
        integer = lambda field: PARSER["ber"](fields[field], 0)[0] if field in fields and fields[field] else 0
        frames.append({"frame_id": frame_id, "cursor": integer(11), "event_id": integer(12),
                       "kind": integer(14), "maniac_event_id": integer(15),
                       "commands_sha256": hashlib.sha256(fields[2]).hexdigest()})
    if pos != len(raw):
        raise ValueError("trailing stack bytes")
    wait_field = event_fields.get(31)
    wait = (PARSER["ber"](blob, wait_field["payload_offset"])[0] if wait_field else 0)
    numbers, common, maps = SAVE_VALUES(path)
    return {"frames": frames, "wait_time": wait, "numbers": numbers,
            "common_self": common, "map_self": maps}


def verify() -> None:
    checks = []
    acceptance_id = ("W02-046" if CONTEXT == "call12330-auto" else
                     "W02-047" if CONTEXT == "call12330-parallel" else
                     "W02-048" if CONTEXT == "call12330-nested" else "W02-028")
    frame_context = CONTEXT.removeprefix("call12330-")
    for runtime in ("original", "player"):
        fixture = ROOT / "build" / f"maniac-{VARIANT}-{runtime}"
        manifest_path = EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if (manifest["save_variant"] != VARIANT or manifest["acceptance_id"] != acceptance_id
                or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA
                or manifest["expected_global_slots_after_event"] != [10, 0, 3]
                or sha(fixture / "manifest.json") != sha(manifest_path)):
            raise ValueError(f"wrong wait fixture: {runtime}")
        for name, expected in manifest["files"].items():
            if sha(fixture / name) != expected:
                raise ValueError(f"wait fixture changed: {runtime}/{name}")
        executable = fixture / "RPG_RT.exe" if runtime == "original" else PLAYER
        for stage in (1, 2):
            trace_path = EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-stage{stage}.json"
            trace = json.loads(trace_path.read_text(encoding="utf-8"))
            attempts_path = EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-stage{stage}-attempts.json"
            attempts = json.loads(attempts_path.read_text(encoding="utf-8"))
            save_path = EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-slot{stage:02d}.lsd"
            if (trace["runtime"] != runtime or trace["stage"] != stage
                    or trace["runtime_sha256"] != (ORIGINAL_SHA if runtime == "original" else CAPTURED_PLAYER_SHA[CONTEXT])
                    or trace["fixture_manifest_sha256"] != sha(manifest_path)
                    or trace["output_save_sha256"] != sha(save_path)
                    or trace["output_save_length"] != save_path.stat().st_size
                    or not attempts or attempts[-1]["result"] != "captured"
                    or trace["input_save_sha256"] != (sha(EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-slot01.lsd") if stage == 2 else None)):
                raise ValueError(f"wait trace provenance changed: {runtime}/{stage}")
            state = frame_state(save_path, frame_context)
            cursors = [frame["cursor"] for frame in state["frames"]]
            kinds = [frame["kind"] for frame in state["frames"]]
            if frame_context == "parallel":
                expected_event_ids = [3, 0] if stage == 1 else [3]
                expected_maniac_ids = [3, 1] if stage == 1 else [3]
            elif frame_context == "nested":
                expected_event_ids = [1, 0, 0] if stage == 1 else [1]
                expected_maniac_ids = [1, 2, 1] if stage == 1 else [1]
            else:
                expected_event_ids = [1, 0] if stage == 1 else [1]
                expected_maniac_ids = [1, 1] if stage == 1 else [1]
            if ([frame["event_id"] for frame in state["frames"]] != expected_event_ids
                    or [frame["maniac_event_id"] for frame in state["frames"]] != expected_maniac_ids):
                raise ValueError(f"saved frame identities differ: {runtime}/{stage}: {state['frames']}")
            if stage == 1:
                expected_cursors = [2, 3] if frame_context == "parallel" else [3, 3, 3] if frame_context == "nested" else [3, 3]
                expected_kinds = [20, 37] if frame_context == "parallel" else [19, 37, 37] if frame_context == "nested" else [19, 37]
                if (cursors != expected_cursors or kinds != expected_kinds or state["wait_time"] <= 0
                        or state["numbers"][:3] != [10, 0, 3]
                        or state["common_self"][0]["numbers_positive_slots"][0] != 42
                        or (frame_context == "nested" and (state["numbers"][4] != 5
                            or [frame["maniac_event_id"] for frame in state["frames"]] != [1, 2, 1]
                            or state["common_self"][1]["numbers_positive_slots"][0] != 24))):
                    raise ValueError(f"child was not waiting at checkpoint: {runtime}: {state}")
            elif (cursors != ([5] if frame_context == "parallel" else [6])
                  or kinds != ([20] if frame_context == "parallel" else [19]) or state["wait_time"] != 0
                  or state["numbers"][:4] != [11, 20, 7, 99]
                  or state["common_self"][0]["numbers_positive_slots"][0] != 42
                  or (frame_context == "nested" and (state["numbers"][4:6] != [6, 88]
                      or state["common_self"][1]["numbers_positive_slots"][0] != 24))):
                raise ValueError(f"restored child/parent outcome differs: {runtime}: {state}")
            checks.append({"runtime": runtime, "stage": stage,
                           "manifest_sha256": sha(manifest_path), "trace_sha256": sha(trace_path),
                           "attempts_sha256": sha(attempts_path),
                           "save_sha256": sha(save_path), "state": state})
    output = EVIDENCE / f"maniac-250921-{VARIANT}-audit.json"
    output.write_text(json.dumps({"reference_sha256": ORIGINAL_SHA,
                                  "captured_player_sha256": CAPTURED_PLAYER_SHA[CONTEXT],
                                  "current_player_sha256": sha(PLAYER),
                                  "runner_sha256": sha(RUNNER),
                                  "fixture_source_sha256": sha(ROOT / "tests/game_interpreter.cpp"),
                                  "fixture_generator_sha256": sha(ROOT / "docs/generate-maniac-original-fixture.py"),
                                  "cases": checks,
                                  "scope": (f"{acceptance_id} {frame_context} parent 12330 selector-0 common call: parallel map event checkpoint, saved frames/cursors/IDs and nonzero wait, common self bank, fresh-process restored child and once-only parent continuation. Other 12330 contexts remain W02-044 children."
                                            if CONTEXT.startswith("call12330-") else
                                            f"W02-028 {CONTEXT} parent 1005 active wait: parallel map event checkpoint, saved frames/cursors and nonzero wait, common self bank, fresh-process restored child and once-only parent continuation. Other contexts have separate evidence.")},
                                 indent=2) + "\n", encoding="utf-8")
    print(f"{output}: {len(checks)} checkpoint and restart records verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--context", choices=("auto", "parallel", "nested", "call12330-auto", "call12330-parallel", "call12330-nested"), default="auto")
    args = parser.parse_args()
    CONTEXT = args.context
    VARIANT = f"call12330-wait-{CONTEXT.removeprefix('call12330-')}" if CONTEXT.startswith("call12330-") else f"common1005-wait-{CONTEXT}"
    if args.capture:
        capture()
    verify()
