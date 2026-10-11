"""Verify opcode 1005 fresh frames, parent continuation and separate self banks."""

from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
RUNNER = ROOT / "build/windows-x64-vs2022-release-local/Release/test_runner.exe"
PLAYER = RUNNER.with_name("Player.exe")
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921",
               "--log-file", "noop-battle-player.log"]
CONTEXTS = {"auto": 1, "parallel": 0, "nested": 1, "battle": 2}
EXPECTED = [31, 31, 42, 42, 77, 77, 123, 333, 90, 0, 0, 123, 333, 90, 0, 0, 0, 0, 0, 333]

def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()

def save_values(path: Path) -> tuple[list[int], list[dict], list[dict]]:
    helper = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    blob = path.read_bytes()
    size, position = helper["ber"](blob, 0)
    if blob[position:position + size] != b"LcfSaveData":
        raise ValueError("wrong save header")
    root = {x["id"]: x for x in helper["chunks"](blob, position + size, len(blob))}
    system = root[101]
    fields = {x["id"]: x for x in helper["chunks"](blob, system["payload_offset"], system["payload_offset"] + system["length"])}
    entry = fields[34]
    raw = blob[entry["payload_offset"]:entry["payload_offset"] + entry["length"]]
    numbers = list(struct.unpack("<" + "i" * (len(raw) // 4), raw))
    if 116 not in root:
        return numbers, [], []  # No self object was allocated in this valid save.
    extra = root[116]
    payload = blob[extra["payload_offset"]:extra["payload_offset"] + extra["length"]]
    children = {x["id"]: x for x in helper["chunks"](payload, 0, len(payload))}
    return numbers, (helper["decode_common_self"](payload, children[1]) if 1 in children else []), (
        helper["decode_map_self_numbers"](payload, children[2]) if 2 in children else [])

def capture() -> None:
    command = [str(RUNNER), "--no-colors",
               "--test-case=Maniac 1005 creates fresh fields without copying a dirty caller"]
    result = subprocess.run(command, cwd=ROOT, check=True, capture_output=True, text=True)
    (EVIDENCE / "maniac-250921-common1005-frame-player-test.json").write_text(
        json.dumps(dict(runner_sha256=sha(RUNNER), command=command[1:], exit_code=result.returncode,
                        output=result.stdout, source_sha256=sha(ROOT / "tests/maniac_common_call.cpp")), indent=2) + "\n", encoding="utf-8")
    for context in CONTEXTS:
        fixture = ROOT / "build" / f"maniac-common1005-frame-{context}"
        variant = f"common1005-{context}"
        subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                        "--save-probe", "--save-variant", variant, "--output", str(fixture)], check=True)
        env = dict(os.environ, MANIAC_COMMON1005_FRAME_DIR=str(fixture),
                   MANIAC_COMMON1005_FRAME_CONTEXT=context)
        subprocess.run([str(RUNNER), "--no-colors",
                        "--test-case=Maniac 1005 frame isolation fixture serialization"], env=env, cwd=ROOT, check=True)
        manifest = json.loads((fixture / "manifest.json").read_text())
        manifest.update(probe="1005 fresh-frame, return-destination and typed self-bank isolation",
                        expected_global_slots_after_event=EXPECTED,
                        acceptance_id="W02-027",
                        verification_status="fixed original/Player frame differential")
        for name in manifest["files"]:
            manifest["files"][name] = sha(fixture / name)
        frozen = EVIDENCE / f"maniac-250921-common1005-frame-{context}-manifest.json"
        frozen.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        shutil.copyfile(frozen, fixture / "manifest.json")
        for runtime in ("original", "player"):
            command = [sys.executable, str(ROOT / "docs/probe-maniac-battle-noop.py"),
                       "--runtime", runtime, "--save-variant", variant, "--fixture", str(fixture),
                       "--output", str(EVIDENCE / f"maniac-250921-common1005-frame-{context}-{runtime}-trace.json"),
                       "--timeout", "30"]
            if runtime == "original":
                command.append("--trace-common1005-frame")
            subprocess.run(command, check=True)

def verify() -> None:
    player_test = EVIDENCE / "maniac-250921-common1005-frame-player-test.json"
    test = json.loads(player_test.read_text())
    if (test["runner_sha256"] != sha(RUNNER) or test["exit_code"] != 0
            or test["source_sha256"] != sha(ROOT / "tests/maniac_common_call.cpp")
            or "10 passed" not in test["output"] or "Status: SUCCESS!" not in test["output"]):
        raise ValueError("Player fresh-frame field check changed or failed")
    checks = []
    for context, mode in CONTEXTS.items():
        fixture = ROOT / "build" / f"maniac-common1005-frame-{context}"
        frozen = EVIDENCE / f"maniac-250921-common1005-frame-{context}-manifest.json"
        manifest = json.loads(frozen.read_text())
        if (manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA or manifest["acceptance_id"] != "W02-027"
                or manifest["expected_global_slots_after_event"] != EXPECTED):
            raise ValueError(f"wrong frame fixture: {context}")
        for name, digest in manifest["files"].items():
            if sha(fixture / name) != digest:
                raise ValueError(f"frame fixture changed: {context}/{name}")
        expected_calls = [1, 2, 1, 2]
        expected_depths = [2, 3, 1, 2]
        if context in ("nested", "battle"):
            expected_calls.insert(0, 4)
            expected_depths = [1, 3, 4, 2, 3]
        for runtime in ("original", "player"):
            prefix = EVIDENCE / f"maniac-250921-common1005-frame-{context}-{runtime}-trace"
            trace_path, save_path = prefix.with_suffix(".json"), prefix.with_suffix(".lsd")
            trace = json.loads(trace_path.read_text())
            if (trace["runtime_sha256"] != (ORIGINAL_SHA if runtime == "original" else sha(PLAYER))
                    or trace["runtime_arguments"] != ([] if runtime == "original" else PLAYER_ARGS)
                    or trace["fixture_manifest_sha256"] != sha(frozen)
                    or trace["save_file_sha256"] != sha(save_path)
                    or trace["save_file_length"] != save_path.stat().st_size):
                raise ValueError(f"frame trace provenance changed: {context}/{runtime}")
            numbers, common, maps = save_values(save_path)
            if numbers[:len(EXPECTED)] != EXPECTED or any(numbers[len(EXPECTED):]):
                raise ValueError(f"parent/child continuation differs: {context}/{runtime}: {numbers[:20]}")
            banks = {bank["common_event_id"]: bank for bank in common}
            if set(banks) != ({1, 2, 3, 4} if context in ("nested", "battle") else {1, 2, 3}):
                raise ValueError(f"wrong common-bank identities: {context}/{runtime}: {banks}")
            for selected, value in ((1, 31), (2, 42), (3, 77)):
                bank = banks[selected]
                if bank["numbers_positive_slots"][0] != value or any(bank["numbers_positive_slots"][1:]):
                    raise ValueError(f"numeric bank inheritance: {context}/{runtime}: {bank}")
                if (bank.get("switch_bytes", []) != ([1] if selected == 3 else [])
                        or bank.get("field_3_observed_utf16_slots", []) != (["P"] if selected == 3 else [])):
                    raise ValueError(f"typed bank inheritance: {context}/{runtime}: {bank}")
            if context in ("nested", "battle"):
                if banks[4]["numbers_positive_slots"][0] != 90 or any(banks[4]["numbers_positive_slots"][1:]) or maps:
                    raise ValueError(f"wrapper bank differs: {context}/{runtime}: {common}, {maps}")
            elif (len(maps) != 1 or maps[0]["map_id"] != 1 or len(maps[0]["events"]) != 1
                    or maps[0]["events"][0]["event_id"] != 1
                    or maps[0]["events"][0]["numbers_positive_slots"][0] != 90
                    or any(maps[0]["events"][0]["numbers_positive_slots"][1:])):
                raise ValueError(f"map caller bank differs: {context}/{runtime}: {maps}")
            if runtime == "original":
                records = trace["dispatch_events"]
                if not trace.get("common1005_frame_instrumented") or len(records) != len(expected_calls):
                    raise ValueError(f"missing frame hooks: {context}")
                for record, selected, depth in zip(records, expected_calls, expected_depths):
                    raw = record["frame_raw"]
                    expected = [0] * 64
                    expected[0:8] = raw[0:8]  # Bound command-list wrapper pointer varies by process.
                    expected[0x20:0x24] = list((depth + 1).to_bytes(4, "little"))
                    expected[0x2c:0x30] = list(selected.to_bytes(4, "little"))
                    expected[0x34] = 1  # Constructor captures current map ID (all fixtures use map 1).
                    expected[0x3d] = 0x25
                    if (record["parameters"] != [selected] or record["owner_mode"] != mode
                            or record["depth_before"] != depth or record["depth_after"] != depth + 1
                            or record["common_event_id"] != selected or record["cursor"] != 0
                            or raw != expected or not any(raw[:8])
                            or record["parent_before"] != record["parent_after"]):
                        raise ValueError(f"fresh frame or caller mutation differs: {context}: {record}")
                parent = records[1 if context in ("nested", "battle") else 0]["parent_before"]
                if (int.from_bytes(bytes(parent[0x18:0x1c]), "little") != 2
                        or int.from_bytes(bytes(parent[0x1c:0x20]), "little") != 20):
                    raise ValueError(f"parent return-destination sentinel missing: {context}")
            checks.append(dict(context=context, runtime=runtime, trace_sha256=sha(trace_path),
                               save_sha256=sha(save_path), numbers=numbers[:20],
                               common_self=common, map_self=maps))
    output = EVIDENCE / "maniac-250921-common1005-frame-audit.json"
    output.write_text(json.dumps(dict(original_sha256=ORIGINAL_SHA, player_sha256=sha(PLAYER),
        runner_sha256=sha(RUNNER), player_frame_test_sha256=sha(player_test), cases=checks,
        scope="W02-027: 250921 Windows fresh 1005 frames and nested caller/typed-self-bank isolation. Original raw 0x40-byte frame fields, unchanged immediate parent, two nested common IDs, second invocation persistence and original/Player parent/child return markers. Wait/save/restart remains W02-028."), indent=2) + "\n", encoding="utf-8")
    print(f"{output}: {len(checks)} frame/continuation outcomes")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    args = parser.parse_args()
    if args.capture:
        capture()
    verify()
