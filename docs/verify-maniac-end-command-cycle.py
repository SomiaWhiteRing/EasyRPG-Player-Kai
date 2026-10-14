"""Capture and compare 250921 End Command parallel round boundaries."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1",
               "--log-file", "end-command-player.log"]
AGENT = r"""
const base = Process.mainModule.base;
const frames = new Map();
Interceptor.attach(base.add(0x4762f), {onEnter(args) {
    const owner = this.context.r13;
    const depth = owner.readS32();
    const frame = owner.add(8).readPointer().add((depth - 1) * 8).readPointer();
    const token = frame.toString();
    if (!frames.has(token)) frames.set(token, frames.size + 1);
    const begin = base.add(0x27a4b8).readPointer();
    const end = base.add(0x27a4c0).readPointer();
    const count = end.sub(begin).toInt32() / 4;
    send({owner_mode: owner.add(0x38).readU8(), frame_depth: depth,
          frame_token: frames.get(token), cursor: frame.add(0x24).readS32(),
          variables: Array.from({length: Math.min(count, 5)}, (_, i)=>begin.add(i*4).readS32())});
}});
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(runtime: str, fixture: Path) -> None:
    name = f"maniac-250921-end-cycle-{runtime}"
    executable = fixture / "RPG_RT.exe" if runtime == "original" else PLAYER
    arguments = [] if runtime == "original" else PLAYER_ARGS
    manifest_path = fixture / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    for file_name, digest in manifest["files"].items():
        if sha256(fixture / file_name) != digest:
            raise ValueError(f"fixture input changed: {file_name}")
    for saved in fixture.glob("Save*.lsd"):
        if saved.name not in manifest["files"]:
            saved.unlink()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(executable), *arguments], cwd=fixture, startupinfo=startup,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    events, errors = [], []
    session = None
    try:
        if runtime == "original":
            import frida
            session = frida.attach(process.pid)
            script = session.create_script(AGENT)

            def on_message(message, _data):
                if message.get("type") == "send":
                    events.append(message["payload"])
                else:
                    errors.append(message)

            script.on("message", on_message)
            script.load()
        deadline = time.monotonic() + 18
        second_save = fixture / "Save02.lsd"
        last_digest, stable_since = None, time.monotonic()
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"{name}: runtime exited {process.returncode}")
            try:
                current = sha256(second_save) if second_save.exists() else None
            except PermissionError:
                current = None
            if current and current == last_digest and time.monotonic() - stable_since > 2:
                break
            if current != last_digest:
                last_digest, stable_since = current, time.monotonic()
            time.sleep(0.1)
        else:
            raise RuntimeError(f"{name}: no stable Save02 within 18 seconds; existing slots="
                               + str([p.name for p in fixture.glob('Save*.lsd')])
                               + f"; end_events={events}; hook_errors={errors}")
        process.kill()
        process.wait()
        if errors:
            raise RuntimeError(f"{name}: native hook errors {errors}")
        saves = {}
        for slot in (1, 2):
            source = fixture / f"Save{slot:02d}.lsd"
            destination = EVIDENCE / f"{name}-slot{slot:02d}.lsd"
            shutil.copyfile(source, destination)
            saves[str(slot)] = {"sha256": sha256(destination), "length": destination.stat().st_size}
        record = {"runtime_sha256": sha256(executable), "runtime_arguments": arguments,
                  "fixture_manifest_sha256": sha256(manifest_path), "saves": saves,
                  "end_events": events,
                  "observation": "Autostart parallel fixture needed no input. Stable first and second round saves copied after termination; original End Command observed with read-only native hook."}
        (EVIDENCE / f"{name}-trace.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print(f"{name}: two rounds saved; End Command events={len(events)}", flush=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        if session is not None:
            try:
                session.detach()
            except Exception:
                pass


def capture_all() -> None:
    fixture = ROOT / "build/maniac-end-cycle"
    subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                    "--save-probe", "--save-variant", "end-command-cycle",
                    "--output", str(fixture)], check=True)
    shutil.copyfile(fixture / "manifest.json", EVIDENCE / "maniac-250921-end-cycle-manifest.json")
    for runtime in ("original", "player"):
        capture(runtime, fixture)


def save_values(path: Path) -> tuple[list[int], list[dict]]:
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
    extra = root[116]
    payload = blob[extra["payload_offset"]:extra["payload_offset"] + extra["length"]]
    children = {x["id"]: x for x in helper["chunks"](payload, 0, len(payload))}
    maps = helper["decode_map_self_numbers"](payload, children[2])
    return numbers, maps


def main() -> None:
    cases = []
    manifest_path = EVIDENCE / "maniac-250921-end-cycle-manifest.json"
    manifest = json.loads(manifest_path.read_text())
    if manifest["save_variant"] != "end-command-cycle" or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA:
        raise ValueError("wrong End Command fixture")
    fixture = ROOT / "build/maniac-end-cycle"
    for file_name, digest in manifest["files"].items():
        if sha256(fixture / file_name) != digest:
            raise ValueError(f"fixture changed: {file_name}")
    for runtime in ("original", "player"):
        name = f"maniac-250921-end-cycle-{runtime}"
        path = EVIDENCE / f"{name}-trace.json"
        trace = json.loads(path.read_text())
        if (trace["runtime_sha256"] != (ORIGINAL_SHA if runtime == "original" else sha256(PLAYER))
                or trace["runtime_arguments"][:3] + trace["runtime_arguments"][-2:] != ([] if runtime == "original" else PLAYER_ARGS)
                or trace["fixture_manifest_sha256"] != sha256(manifest_path)):
            raise ValueError(f"runtime/fixture provenance changed: {name}")
        if runtime == "original":
            events = trace["end_events"]
            if len(events) < 2 or any(event["owner_mode"] != 0 or event["frame_depth"] != 1
                                     or event["frame_token"] != 1 or event["cursor"] != 7 for event in events[:2]):
                raise ValueError(f"original End Command single frame identity changed: {events}")
            if [event["variables"][1:3] for event in events[:2]] != [[1, 1], [2, 2]]:
                raise ValueError("original End Command round order changed")
        elif trace["end_events"]:
            raise ValueError("Player trace must not claim original native hook events")
        rounds = []
        for slot in (1, 2):
            save = EVIDENCE / f"{name}-slot{slot:02d}.lsd"
            record = trace["saves"][str(slot)]
            if record["sha256"] != sha256(save) or record["length"] != save.stat().st_size:
                raise ValueError(f"round save changed: {name}/{slot}")
            numbers, maps = save_values(save)
            if numbers[:2] != [slot, slot] or any(numbers[2:]):
                raise ValueError(f"counter/self copy/poison changed: {name}/{slot}: {numbers}")
            bank = maps[0]["events"][0]
            if (len(maps) != 1 or maps[0]["map_id"] != 1 or len(maps[0]["events"]) != 1
                    or bank["event_id"] != 1 or bank["numbers_positive_slots"][:1] != [slot]
                    or any(bank["numbers_positive_slots"][1:])):
                raise ValueError(f"parallel self bank changed: {maps}")
            rounds.append({"slot": slot, "numbers": numbers, "map_self": maps})
        cases.append({"runtime": runtime, "rounds": rounds})
    output = EVIDENCE / "maniac-250921-end-cycle-audit.json"
    audit = {"original_sha256": ORIGINAL_SHA, "player_sha256": sha256(PLAYER), "cases": cases,
             "scope": "250921 Windows map parallel mode 0 single frame End Command at the LCF file terminator. Both original and Player produce two rounds with counters and same self bank advancing 1 to 2. Original native hook observes same frame twice; Player reaches the list boundary without an explicit opcode 0 object. Nested/mode1 and battle/mode2 remain separate."}
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: two runtimes x two rounds verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    args = parser.parse_args()
    if args.capture:
        capture_all()
    main()
