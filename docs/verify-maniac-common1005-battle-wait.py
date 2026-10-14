"""Capture 1005/12330 battle wait, save boundary and fresh-process map restoration."""

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
VARIANT = "common1005-wait-battle"
CONTEXT = "1005"
ACCEPTANCE_ID = "W02-036"
CALL_RVA = 0x1D550
CALL_EVENT = "call1005"
CAPTURED_PLAYER_SHA = "6dff2314eb92269b91326c598118edc3870196c453421d28e9e4a3307f67d516"
RUNNER = ROOT / "build/windows-x64-vs2022-release-local/Release/test_runner.exe"
PLAYER = RUNNER.with_name("Player.exe")
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
FRAME_STATE = runpy.run_path(str(ROOT / "docs/verify-maniac-common1005-wait.py"))["frame_state"]

BATTLE_AGENT = r"""
const base = Process.mainModule.base;
function top(owner) {
    const depth = owner.readS32();
    const mode = owner.add(0x38).readU8();
    if (depth < 1) return {mode, depth};
    const frame = owner.add(8).readPointer().add((depth-1)*8).readPointer();
    return {mode, depth, kind:frame.add(0x3d).readU8(),
        common_id:frame.add(0x2c).readS32(), cursor:frame.add(0x24).readS32()};
}
let saves = 0, pendingSave = null;
Interceptor.attach(base.add(__CALL_RVA__), {
    onEnter(args) {this.owner=args[0]; this.before=top(args[0]);},
    onLeave() {send({event:__CALL_EVENT__, tick_ms:Date.now(), before:this.before, after:top(this.owner)});}
});
Interceptor.attach(base.add(0x45660), {
    onEnter(args) {
        const owner=args[0];
        const state=top(owner);
        if (state.mode !== 2 || state.depth < 1) return;
        const frame=owner.add(8).readPointer().add((state.depth-1)*8).readPointer();
        const list=frame.readPointer().readPointer();
        if (state.cursor < 0 || state.cursor >= list.readS32()) return;
        const command=list.add(8).readPointer().add(state.cursor*8).readPointer();
        const opcode=command.readS32();
        if (opcode === 3002) pendingSave=state;
        if (state.kind !== 37) return;
        send({event:'child_dispatch', tick_ms:Date.now(), state, opcode});
    },
    onLeave() {
        if (pendingSave !== null) pendingSave=null;
    }
});
Interceptor.attach(base.add(0x24f10), {
    onEnter(args) {
        const command=args[2], count=command.add(0x20).readU32();
        const data=command.add(0x18).readPointer();
        this.record={event:'save_command', tick_ms:Date.now(), sequence:++saves,
            state:pendingSave,
            parameters:Array.from({length:count},(_,i)=>data.add(4*i).readS32())};
    },
    onLeave() {send(this.record);}
});
"""


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def path_for(runtime: str, name: str) -> Path:
    return EVIDENCE / f"maniac-250921-{VARIANT}-{runtime}-{name}"


def wait_for_save(process: subprocess.Popen, target: Path, runtime: str, stage: int) -> None:
    deadline = time.monotonic() + 50
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
            return
        if digest != previous:
            previous = digest
            stable_since = time.monotonic()
        time.sleep(0.1)
    raise RuntimeError(f"{runtime} stage {stage} produced no stable {target.name}")


def capture_stage(fixture: Path, runtime: str, stage: int) -> None:
    attempt_path = path_for(runtime, f"stage{stage}-attempts.json")
    attempts = []
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
            attempt_path.write_text(json.dumps(attempts, indent=2) + "\n", encoding="utf-8")


def capture_stage_once(fixture: Path, runtime: str, stage: int) -> None:
    target_slot = 4 if stage == 1 else 3
    for slot in ((1, 2, 3, 4) if stage == 1 else (2, 3, 4)):
        (fixture / f"Save{slot:02d}.lsd").unlink(missing_ok=True)
    input_save_sha256 = None
    if stage == 2:
        input_save_sha256 = sha(fixture / "Save01.lsd")
        if input_save_sha256 != sha(path_for(runtime, "slot01.lsd")):
            raise RuntimeError(f"{runtime} restart input differs from frozen Save01")
    executable = fixture / "RPG_RT.exe" if runtime == "original" else PLAYER
    arguments = ([] if runtime == "original" else
                 ["--new-game", "--patch-maniac", "1"] if stage == 1 else
                 ["--load-game-id", "1", "--patch-maniac", "1"])
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(executable), *arguments], cwd=fixture,
                               startupinfo=startup, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    events = []
    errors = []
    session = None
    try:
        if runtime == "original" and stage == 1:
            import frida
            session = frida.attach(process.pid)
            script = session.create_script(BATTLE_AGENT.replace("__CALL_RVA__", hex(CALL_RVA))
                                           .replace("__CALL_EVENT__", json.dumps(CALL_EVENT)))
            def on_message(message, data):
                if message.get("type") == "send":
                    events.append(message["payload"])
                else:
                    errors.append(message)
            script.on("message", on_message)
            script.load()
        wait_for_save(process, fixture / f"Save{target_slot:02d}.lsd", runtime, stage)
    except RuntimeError as error:
        path_for(runtime, f"stage{stage}-failure.json").write_text(json.dumps({
            "runtime": runtime, "stage": stage, "result": str(error),
            "runtime_sha256": sha(executable), "runtime_arguments": arguments,
            "fixture_manifest_sha256": sha(fixture / "manifest.json"),
            "original_native_events": events, "trace_errors": errors,
            "existing_save_slots": [slot for slot in (1, 2, 3, 4)
                                    if (fixture / f"Save{slot:02d}.lsd").exists()],
        }, indent=2) + "\n", encoding="utf-8")
        raise
    finally:
        if process.poll() is None:
            process.kill()
        process.wait()
        if session is not None:
            session.detach()
    if errors:
        raise RuntimeError(f"original trace errors: {errors}")
    frozen = {}
    slots = (1, 2, 4) if stage == 1 else (3,)
    for slot in slots:
        source = fixture / f"Save{slot:02d}.lsd"
        if not source.is_file():
            raise RuntimeError(f"missing stage {stage} Save{slot:02d}")
        destination = path_for(runtime, f"slot{slot:02d}.lsd")
        shutil.copyfile(source, destination)
        frozen[str(slot)] = {"sha256": sha(destination), "length": destination.stat().st_size,
                             "source_mtime_ns": source.stat().st_mtime_ns}
    trace_path = path_for(runtime, f"stage{stage}.json")
    trace_path.write_text(json.dumps({
        "runtime": runtime, "stage": stage, "runtime_sha256": sha(executable),
        "runtime_arguments": arguments, "fixture_manifest_sha256": sha(fixture / "manifest.json"),
        "input_save_sha256": input_save_sha256,
        "saved_slots": frozen, "original_native_events": events,
        "unexpected_post_restart_slots": ([slot for slot in (2, 4) if (fixture / f"Save{slot:02d}.lsd").exists()]
                                          if stage == 2 else []),
        "observation": "Fresh hidden process, no input; frozen saves after target unchanged for one second; process terminated.",
    }, indent=2) + "\n", encoding="utf-8")
    print(f"{trace_path}: {', '.join(frozen)}; {len(events)} native events")


def capture() -> None:
    for runtime in ("original", "player"):
        fixture = ROOT / "build" / f"maniac-{VARIANT}-{runtime}"
        subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                        "--save-probe", "--save-variant", VARIANT, "--output", str(fixture)], check=True)
        shutil.copyfile(fixture / "manifest.json", path_for(runtime, "manifest.json"))
        capture_stage(fixture, runtime, 1)
        capture_stage(fixture, runtime, 2)


def verify() -> None:
    cases = []
    for runtime in ("original", "player"):
        fixture = ROOT / "build" / f"maniac-{VARIANT}-{runtime}"
        manifest_path = path_for(runtime, "manifest.json")
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if (manifest["save_variant"] != VARIANT or manifest["acceptance_id"] != ACCEPTANCE_ID
                or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA
                or manifest["expected_global_slots_after_event"] != [0, 0, 3, 0, 0, 0, 0, 0, 0, 109]
                or sha(fixture / "manifest.json") != sha(manifest_path)):
            raise ValueError(f"wrong fixture: {runtime}")
        for name, expected in manifest["files"].items():
            if sha(fixture / name) != expected:
                raise ValueError(f"fixture changed: {runtime}/{name}")
        executable = fixture / "RPG_RT.exe" if runtime == "original" else PLAYER
        states = {}
        for stage, slots in ((1, (1, 2, 4)), (2, (3,))):
            trace_path = path_for(runtime, f"stage{stage}.json")
            trace = json.loads(trace_path.read_text(encoding="utf-8"))
            attempts_path = path_for(runtime, f"stage{stage}-attempts.json")
            attempts = json.loads(attempts_path.read_text(encoding="utf-8"))
            if (trace["runtime"] != runtime or trace["stage"] != stage
                    or trace["runtime_sha256"] != (ORIGINAL_SHA if runtime == "original" else CAPTURED_PLAYER_SHA)
                    or trace["fixture_manifest_sha256"] != sha(manifest_path)
                    or not attempts or attempts[-1]["result"] != "captured"
                    or trace["input_save_sha256"] != (sha(path_for(runtime, "slot01.lsd")) if stage == 2 else None)
                    or trace["unexpected_post_restart_slots"]):
                raise ValueError(f"trace provenance changed: {runtime}/{stage}")
            for slot in slots:
                save_path = path_for(runtime, f"slot{slot:02d}.lsd")
                frozen = trace["saved_slots"][str(slot)]
                if frozen["sha256"] != sha(save_path) or frozen["length"] != save_path.stat().st_size:
                    raise ValueError(f"save changed: {runtime}/{slot}")
                states[slot] = FRAME_STATE(save_path)
            cases.append({"runtime": runtime, "stage": stage,
                          "manifest_sha256": sha(manifest_path), "trace_sha256": sha(trace_path),
                          "attempts_sha256": sha(attempts_path),
                          "saves": {str(slot): {"sha256": sha(path_for(runtime, f"slot{slot:02d}.lsd")),
                                                "state": states[slot]} for slot in slots}})
        # The native battle/common stack is deliberately compared with what
        # the original LSD can actually carry. Fresh load returns to map.
        first_stage = json.loads(path_for(runtime, "stage1.json").read_text(encoding="utf-8"))
        slot_times = first_stage["saved_slots"]
        if not (4_500_000_000 <= slot_times["2"]["source_mtime_ns"] - slot_times["1"]["source_mtime_ns"]
                <= 7_000_000_000
                and slot_times["4"]["source_mtime_ns"] >= slot_times["2"]["source_mtime_ns"]):
            raise ValueError(f"battle wait duration/order differs: {runtime}: {slot_times}")
        for slot, cursor, child_count, parent_count in ((1, 3, 0, 0), (2, 3, 1, 0),
                                                         (4, 3, 1, 1), (3, 4, 0, 0)):
            state = states[slot]
            if ([frame["cursor"] for frame in state["frames"]] != [cursor]
                    or [frame["kind"] for frame in state["frames"]] != [19]
                    or state["numbers"][2] != 3 or state["numbers"][3] != child_count
                    or state["numbers"][4] != parent_count
                    or state["numbers"][9] != 109
                    or state["common_self"][0]["numbers_positive_slots"][0] != 42):
                raise ValueError(f"battle save boundary differs: {runtime}/slot{slot}: {state}")
        if runtime == "original":
            events = json.loads(path_for(runtime, "stage1.json").read_text(encoding="utf-8"))["original_native_events"]
            calls = [event for event in events if event["event"] == CALL_EVENT]
            saves = [event for event in events if event["event"] == "save_command"]
            dispatch = [event for event in events if event["event"] == "child_dispatch"]
            if (len(calls) != 1 or len(saves) != 3 or len(dispatch) != 7
                    or calls[0]["before"]["kind"] != 64
                    or calls[0]["after"]["mode"] != 2 or calls[0]["after"]["kind"] != 37
                    or calls[0]["after"]["common_id"] != 1
                    or calls[0]["after"]["depth"] != calls[0]["before"]["depth"] + 1
                    or [event["parameters"][1] for event in saves[:3]] != [1, 2, 4]
                    or [event["state"]["mode"] for event in saves[:3]] != [2, 2, 2]
                    or [event["state"]["kind"] for event in saves[:3]] != [37, 37, 64]
                    or [event["opcode"] for event in dispatch[:7]] !=
                       [3036, 10220, 3002, 11410, 10220, 3002, 10]
                    or not 4500 <= saves[1]["tick_ms"] - saves[0]["tick_ms"] <= 7000):
                raise ValueError(f"native battle/child trace differs: {events}")
    audit = EVIDENCE / f"maniac-250921-{VARIANT}-audit.json"
    audit.write_text(json.dumps({
        "reference_sha256": ORIGINAL_SHA, "captured_player_sha256": CAPTURED_PLAYER_SHA,
        "current_player_sha256": sha(PLAYER), "acceptance_id": ACCEPTANCE_ID,
        "runner_sha256": sha(RUNNER),
        "fixture_source_sha256": sha(ROOT / "tests/game_interpreter.cpp"),
        "fixture_generator_sha256": sha(ROOT / "docs/generate-maniac-original-fixture.py"),
        "verifier_sha256": sha(Path(__file__)), "cases": cases,
        "scope": f"Battle {CONTEXT} child saves before and after wait; native battle/common stack is omitted from original LSD, and fresh-process Save01 resumes map only. Player matches this compatibility boundary; broader Player full-state persistence remains W11-035.",
    }, indent=2) + "\n", encoding="utf-8")
    print(f"{audit}: four stage records verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--context", choices=("1005", "12330"), default="1005")
    args = parser.parse_args()
    CONTEXT = args.context
    if CONTEXT == "12330":
        VARIANT = "call12330-wait-battle"
        ACCEPTANCE_ID = "W02-049"
        CALL_RVA = 0x252F0
        CALL_EVENT = "call12330"
        CAPTURED_PLAYER_SHA = "15c0c511d88affc27b6a72047b586da43fc1fc23d02572fe892718617c5c8cc9"
    if args.capture:
        capture()
    verify()
