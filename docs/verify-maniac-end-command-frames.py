"""Compare 250921 nested and autostart End Command frame boundaries."""

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
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921",
               "--log-file", "end-command-frames-player.log"]
AGENT = r"""
const base = Process.mainModule.base;
function sample(context, stage) {
    const owner = context.r13;
    const depth = owner.readS32();
    const frame = depth > 0 ? owner.add(8).readPointer().add((depth-1)*8).readPointer() : null;
    const begin = base.add(0x27a4b8).readPointer();
    const end = base.add(0x27a4c0).readPointer();
    const count = end.sub(begin).toInt32()/4;
    send({stage, owner_mode: owner.add(0x38).readU8(), frame_depth: depth,
          cursor: frame === null ? null : frame.add(0x24).readS32(),
          variables: Array.from({length: Math.min(count, 5)}, (_,i)=>begin.add(i*4).readS32())});
}
for (const [address,stage] of [[0x4762f,'entry'],[0x4766a,'after_cleanup']]) {
    Interceptor.attach(base.add(address), {onEnter(args) {sample(this.context,stage);}});
}
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(mode: str, runtime: str, fixture: Path) -> None:
    name = f"maniac-250921-end-frames-{mode}-{runtime}"
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
            raise RuntimeError(f"{name}: no stable Save02; events={events}; errors={errors}")
        process.kill()
        process.wait()
        if errors:
            raise RuntimeError(f"{name}: native hook errors {errors}")
        saves = {}
        for slot in (1, 2):
            destination = EVIDENCE / f"{name}-slot{slot:02d}.lsd"
            shutil.copyfile(fixture / f"Save{slot:02d}.lsd", destination)
            saves[str(slot)] = {"sha256": sha256(destination), "length": destination.stat().st_size}
        record = {"runtime_sha256": sha256(executable), "runtime_arguments": arguments,
                  "fixture_manifest_sha256": sha256(manifest_path), "saves": saves,
                  "end_events": events,
                  "observation": "No input required. Native hook only reads original End Command entry and post-cleanup state. Both rounds frozen after termination."}
        (EVIDENCE / f"{name}-trace.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print(f"{name}: two rounds saved; native observations={len(events)}", flush=True)
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
    for mode in ("auto", "parallel"):
        fixture = ROOT / f"build/maniac-end-frames-{mode}"
        subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                        "--save-probe", "--save-variant", f"end-command-frames-{mode}",
                        "--output", str(fixture)], check=True)
        shutil.copyfile(fixture / "manifest.json", EVIDENCE / f"maniac-250921-end-frames-{mode}-manifest.json")
        for runtime in ("original", "player"):
            capture(mode, runtime, fixture)


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
    extra = root[116]
    payload = blob[extra["payload_offset"]:extra["payload_offset"] + extra["length"]]
    children = {x["id"]: x for x in helper["chunks"](payload, 0, len(payload))}
    return numbers, helper["decode_common_self"](payload, children[1]), helper["decode_map_self_numbers"](payload, children[2])


def verify_events(mode: str, events: list[dict]) -> None:
    expected_mode = 1 if mode == "auto" else 0
    if any(e["owner_mode"] != expected_mode for e in events):
        raise ValueError(f"wrong owner mode: {events}")
    entries = [e for e in events if e["stage"] == "entry"]
    if [e["frame_depth"] for e in entries[:4]] != [2, 1, 2, 1]:
        raise ValueError(f"wrong nested/parent round order: {events}")
    if [e["cursor"] for e in entries[:4]] != [4, 8, 4, 8]:
        raise ValueError(f"wrong list terminator cursor: {events}")
    if [e["variables"][1:4] for e in entries[:4]] != [[1, 0, 1], [1, 1, 1], [2, 1, 2], [2, 2, 2]]:
        raise ValueError(f"wrong continuation order: {events}")
    for slot in (1, 2):
        index = next(i for i, e in enumerate(events) if e["stage"] == "entry" and e["frame_depth"] == 2 and e["variables"][1] == slot)
        if (events[index + 1]["stage"] != "after_cleanup" or events[index + 1]["frame_depth"] != 1
                or events[index + 1]["cursor"] != 3
                or events[index + 1]["variables"] != events[index]["variables"]):
            raise ValueError(f"nested frame not popped: {events}")
        if mode == "auto":
            index = next(i for i, e in enumerate(events) if e["stage"] == "entry" and e["frame_depth"] == 1 and e["variables"][1] == slot)
            if events[index + 1]["stage"] != "after_cleanup" or events[index + 1]["frame_depth"] != 0:
                raise ValueError(f"autostart frame not popped: {events}")


def main() -> None:
    cases = []
    for mode in ("auto", "parallel"):
        manifest_path = EVIDENCE / f"maniac-250921-end-frames-{mode}-manifest.json"
        manifest = json.loads(manifest_path.read_text())
        if manifest["save_variant"] != f"end-command-frames-{mode}" or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA:
            raise ValueError("wrong End Command fixture")
        fixture = ROOT / f"build/maniac-end-frames-{mode}"
        for file_name, digest in manifest["files"].items():
            if sha256(fixture / file_name) != digest:
                raise ValueError(f"fixture changed: {file_name}")
        for runtime in ("original", "player"):
            name = f"maniac-250921-end-frames-{mode}-{runtime}"
            trace = json.loads((EVIDENCE / f"{name}-trace.json").read_text())
            if (trace["runtime_sha256"] != (ORIGINAL_SHA if runtime == "original" else sha256(PLAYER))
                    or trace["runtime_arguments"] != ([] if runtime == "original" else PLAYER_ARGS)
                    or trace["fixture_manifest_sha256"] != sha256(manifest_path)):
                raise ValueError(f"runtime/fixture provenance changed: {name}")
            if runtime == "original":
                verify_events(mode, trace["end_events"])
            elif trace["end_events"]:
                raise ValueError("Player must not claim original native observations")
            rounds = []
            for slot in (1, 2):
                save = EVIDENCE / f"{name}-slot{slot:02d}.lsd"
                record = trace["saves"][str(slot)]
                if record["sha256"] != sha256(save) or record["length"] != save.stat().st_size:
                    raise ValueError(f"save changed: {name}/{slot}")
                numbers, common, maps = save_values(save)
                if numbers[:3] != [slot] * 3 or any(numbers[3:]):
                    raise ValueError(f"wrong parent continuation: {name}/{slot}: {numbers}")
                if (len(common) != 1 or common[0]["common_event_id"] != 3
                        or common[0]["numbers_positive_slots"][:1] != [slot]
                        or any(common[0]["numbers_positive_slots"][1:])):
                    raise ValueError(f"wrong common self bank: {common}")
                if (len(maps) != 1 or maps[0]["map_id"] != 1 or len(maps[0]["events"]) != 1
                        or maps[0]["events"][0]["event_id"] != 1
                        or maps[0]["events"][0]["numbers_positive_slots"][:1] != [slot]
                        or any(maps[0]["events"][0]["numbers_positive_slots"][1:])):
                    raise ValueError(f"wrong map self bank: {maps}")
                rounds.append({"slot": slot, "numbers": numbers, "common_self": common, "map_self": maps})
            cases.append({"mode": mode, "runtime": runtime, "rounds": rounds})
    output = EVIDENCE / "maniac-250921-end-frames-audit.json"
    audit = {"original_sha256": ORIGINAL_SHA, "player_sha256": sha256(PLAYER), "cases": cases,
             "scope": "250921 Windows mode 0 nested and mode 1 nested/single End Command at the standard LCF terminator. Parent continuation and separate map/common self banks match across two rounds. Original observes depth 2 to 1, and mode 1 depth 1 to 0. Battle mode 2 remains separate."}
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: two modes x two runtimes x two rounds verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    args = parser.parse_args()
    if args.capture:
        capture_all()
    main()
