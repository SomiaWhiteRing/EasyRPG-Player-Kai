"""Launch a frozen event fixture and capture its save, optionally tracing native execution.

The fixture autostarts without input. The probe sends no window messages or keyboard input.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
REFERENCE_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1",
               "--log-file", "noop-battle-player.log"]
AGENT = r"""
const base = Process.mainModule.base;
Interceptor.attach(base.add(0x45660), {
    onEnter(args) {
        this.owner = args[0];
        this.record = null;
        const count = this.owner.readS32();
        if (count <= 0) return;
        const frame = this.owner.add(8).readPointer().add((count - 1) * 8).readPointer();
        const list = frame.readPointer().readPointer();
        const cursor = frame.add(0x24).readS32();
        if (cursor < 0 || cursor >= list.readS32()) return;
        const command = list.add(8).readPointer().add(cursor * 8).readPointer();
        const opcode = command.readS32();
        if (![3024, 3034, 3035].includes(opcode)) return;
        this.frame = frame;
        this.record = {opcode, cursor_before: cursor,
            caller_rva: this.returnAddress.sub(base).toString(),
            owner_mode: this.owner.add(0x38).readU8()};
    },
    onLeave(result) {
        if (!this.record) return;
        this.record.cursor_after = this.frame.add(0x24).readS32();
        this.record.result_al = result.toUInt32() & 255;
        send(this.record);
    }
});
"""
SAVE_AGENT = r"""
const base = Process.mainModule.base;
let active = null;
function values() {
    const begin = base.add(0x27a4b8).readPointer();
    const end = base.add(0x27a4c0).readPointer();
    const count = end.sub(begin).toInt32() / 4;
    return Array.from({length: Math.min(count, 12)}, (_, i) => begin.add(i * 4).readS32());
}
Interceptor.attach(base.add(0x24f10), {
    onEnter(args) {
        const command = args[2];
        const count = command.add(0x20).readU32();
        const data = command.add(0x18).readPointer();
        this.record = {parameters: Array.from({length: count}, (_, i) => data.add(i * 4).readS32()),
            values_before: values(), save_slots: []};
        active = this.record;
    },
    onLeave() {
        this.record.values_after = values();
        send(this.record);
        active = null;
    }
});
Interceptor.attach(base.add(0x82cb0), {
    onEnter(args) { if (active) active.save_slots.push(args[0].toInt32()); }
});
"""
END_NEST_AGENT = r"""
const base = Process.mainModule.base;
const summary = {end_nest_dispatch_count:0, owner_modes:{}, first_cursor:null, last_cursor:null};
Interceptor.attach(base.add(0x463e2), {onEnter() {
    const owner = this.context.r13;
    const depth = owner.readS32();
    if (depth < 1) return;
    const frame = owner.add(8).readPointer().add((depth-1)*8).readPointer();
    const cursor = frame.add(0x24).readS32();
    summary.end_nest_dispatch_count++;
    const mode = owner.add(0x38).readU8().toString();
    summary.owner_modes[mode] = (summary.owner_modes[mode] || 0) + 1;
    if (summary.first_cursor === null) summary.first_cursor = cursor;
    summary.last_cursor = cursor;
}});
Interceptor.attach(base.add(0x24f10), {onEnter() {
    if (summary.end_nest_dispatch_count > 0) send(summary);
}});
"""
OPCODE40_AGENT = END_NEST_AGENT.replace(
    "const summary = {end_nest_dispatch_count:0, owner_modes:{}, first_cursor:null, last_cursor:null};",
    "const summary = {opcode40_dispatch_count:0, owner_modes:{}, first_cursor:null, last_cursor:null};"
).replace(
    "    summary.end_nest_dispatch_count++;",
    "    if (cursor < 1) return;\n"
    "    const list = frame.readPointer().readPointer();\n"
    "    const command = list.add(8).readPointer().add((cursor-1)*8).readPointer();\n"
    "    if (command.readS32() !== 40) return;\n"
    "    summary.opcode40_dispatch_count++;"
).replace("summary.end_nest_dispatch_count > 0", "summary.opcode40_dispatch_count > 0")
COMMON1005_AGENT = r"""
const base = Process.mainModule.base;
Interceptor.attach(base.add(0x1d550), {
    onEnter(args) {
        this.owner = args[0];
        const command = args[2];
        const count = command.add(0x20).readU32();
        const data = command.add(0x18).readPointer();
        this.record = {parameters: Array.from({length: count}, (_, i) => data.add(i*4).readS32()),
            owner_mode: this.owner.add(0x38).readU8(), depth_before: this.owner.readS32()};
    },
    onLeave() {
        this.record.depth_after = this.owner.readS32();
        const frame = this.owner.add(8).readPointer().add((this.record.depth_after-1)*8).readPointer();
        this.record.common_event_id = frame.add(0x2c).readS32();
        this.record.frame_kind = frame.add(0x3d).readU8();
        this.record.cursor = frame.add(0x24).readS32();
        send(this.record);
    }
});
"""
COMMON1005_FRAME_AGENT = COMMON1005_AGENT.replace(
    "    },\n    onLeave() {",
    "        this.parent = this.owner.add(8).readPointer().add((this.record.depth_before-1)*8).readPointer();\n"
    "        this.record.parent_before = Array.from(new Uint8Array(this.parent.readByteArray(0x40)));\n"
    "    },\n    onLeave() {").replace(
    "        send(this.record);",
    "        this.record.frame_raw = Array.from(new Uint8Array(frame.readByteArray(0x40)));\n"
    "        this.record.parent_after = Array.from(new Uint8Array(this.parent.readByteArray(0x40)));\n"
    "        send(this.record);")
COMMON1005_TEXT_AGENT = COMMON1005_AGENT.replace(
    "    },\n    onLeave() {",
    "        const text = command.add(0x10).readPointer();\n"
    "        this.record.text = text.isNull() ? '' : text.readUtf16String();\n"
    "    },\n    onLeave() {")
END_COMMAND_BATTLE_AGENT = r"""
const base = Process.mainModule.base;
const summary = {events: [], input_updates: 0};
let active = false;
function record(stage, context) {
    const owner = context.r13;
    const depth = owner.readS32();
    if (owner.add(0x38).readU8() !== 2) return;
    const frame = depth > 0 ? owner.add(8).readPointer().add((depth-1)*8).readPointer() : null;
    let cursor = null, opcode = null;
    if (frame !== null) {
        cursor = frame.add(0x24).readS32();
        const list = frame.readPointer().readPointer();
        if (cursor > 0 && cursor <= list.readS32()) {
            const command = list.add(8).readPointer().add((cursor-1)*8).readPointer();
            opcode = command.readS32();
        }
    }
    if (stage === 'entry' && opcode !== 0) return;
    if (stage === 'entry') active = true;
    if (active) summary.events.push({stage, frame_depth:depth, cursor, opcode});
}
for (const [rva, stage] of [[0x4762f,'entry'],[0x47662,'before_cleanup'],
    [0x4766a,'after_cleanup'],[0x47675,'first_update_call'],[0x4767a,'second_update_call']]) {
    Interceptor.attach(base.add(rva), {onEnter() {record(stage, this.context);}});
}
Interceptor.attach(base.add(0x96840), {onEnter() {
    if (!active) return;
    summary.input_updates++;
    if (summary.input_updates === 2 && !summary.reported) {
        summary.reported = true;
        send(summary);
        active = false;
    }
}});
"""

def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", choices=("original", "player"), required=True)
    parser.add_argument("--trace-dispatch", action="store_true")
    parser.add_argument("--trace-save", action="store_true")
    parser.add_argument("--trace-end-nest", action="store_true")
    parser.add_argument("--trace-opcode40", action="store_true")
    parser.add_argument("--trace-common1005", action="store_true")
    parser.add_argument("--trace-common1005-frame", action="store_true")
    parser.add_argument("--trace-common1005-text", action="store_true")
    parser.add_argument("--trace-end-command-battle", action="store_true")
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--stability-seconds", type=float, default=1.0,
                        help="Minimum unchanged-save interval before capture")
    parser.add_argument("--save-variant", choices=("noop-battle", "noop-long-skip", "end-nest-long-skip", "end-nest-auto-common", "end-nest-battle", "opcode40-long-skip", "opcode40-auto-common", "opcode40-battle", "common1005-auto", "common1005-parallel", "common1005-nested", "common1005-battle", "common1005-wait-auto", "common1005-wait-parallel", "common1005-wait-nested", "common1005-wait-battle", "common1005-generated-auto", "common1005-generated-parallel", "common1005-generated-nested", "common1005-generated-battle", "common1005-modes-auto", "common1005-modes-parallel", "common1005-modes-nested", "common1005-modes-battle", "end-command-battle", "save-boundaries", "load-boundaries-seed", "load-boundaries", "js-host-negative", "js-host-edges"), default="noop-battle")
    parser.add_argument("--no-log", action="store_true", help="Keep the earlier four-argument Player CLI")
    parser.add_argument("--fixture", type=Path, default=ROOT / "build/maniac-250921-noop-battle")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    fixture = args.fixture.resolve()
    manifest_path = fixture / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if manifest["save_variant"] != args.save_variant or manifest["files"]["RPG_RT.exe"] != REFERENCE_SHA256:
        raise ValueError("wrong original fixture")
    for name, digest in manifest["files"].items():
        if sha256(fixture / name) != digest:
            raise ValueError(f"fixture input changed: {name}")
    executable = (fixture / "RPG_RT.exe" if args.runtime == "original" else
                  ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe")
    runtime_args = [] if args.runtime == "original" else PLAYER_ARGS[:-2] if args.no_log else PLAYER_ARGS
    slot = manifest["expected_save_slot"]
    save = fixture / f"Save{slot:02d}.lsd"
    for saved in fixture.glob("Save*.lsd"):
        if saved.name not in manifest["files"] and saved.name in {"Save01.lsd", "Save02.lsd", "Save05.lsd", "Save07.lsd", "Save08.lsd", "Save09.lsd"}:
            saved.unlink()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(executable), *runtime_args], cwd=fixture,
                               startupinfo=startup, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    events = []
    errors = []
    session = None
    try:
        if args.trace_dispatch or args.trace_save or args.trace_end_nest or args.trace_opcode40 or args.trace_common1005 or args.trace_common1005_frame or args.trace_common1005_text or args.trace_end_command_battle:
            if args.runtime != "original":
                raise ValueError("native tracing is only defined for original 250921")
            if sum((args.trace_dispatch, args.trace_save, args.trace_end_nest, args.trace_opcode40, args.trace_common1005, args.trace_common1005_frame, args.trace_common1005_text, args.trace_end_command_battle)) > 1:
                raise ValueError("choose one trace mechanism")
            import frida
            session = frida.attach(process.pid)
            script = session.create_script(END_COMMAND_BATTLE_AGENT if args.trace_end_command_battle else COMMON1005_FRAME_AGENT if args.trace_common1005_frame else COMMON1005_TEXT_AGENT if args.trace_common1005_text else COMMON1005_AGENT if args.trace_common1005 else OPCODE40_AGENT if args.trace_opcode40 else END_NEST_AGENT if args.trace_end_nest else SAVE_AGENT if args.trace_save else AGENT)
            def on_message(message, data):
                if message.get("type") == "send":
                    events.append(message["payload"])
                    print(json.dumps(message["payload"]), flush=True)
                else:
                    errors.append(message)
                    print(json.dumps(message), flush=True)
            script.on("message", on_message)
            script.load()
        print(f"Runtime PID {process.pid}; fixture autostarts.", flush=True)
        deadline = time.monotonic() + args.timeout
        last_hash = None
        stable_since = time.monotonic()
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"runtime exited with {process.returncode}")
            try:
                current_hash = sha256(save) if save.exists() else None
            except PermissionError:
                current_hash = None
            if current_hash and current_hash == last_hash:
                if time.monotonic() - stable_since >= args.stability_seconds:
                    break
            else:
                last_hash = current_hash
                stable_since = time.monotonic()
            time.sleep(0.1)
        else:
            raise RuntimeError("battle fixture did not create a stable save")
        process.kill()
        process.wait()
        if errors:
            raise RuntimeError(f"dispatch hook errors: {errors}")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        output_save = args.output.with_suffix(".lsd")
        output_save.write_bytes(save.read_bytes())
        trace = {"reference_sha256": REFERENCE_SHA256,
                 "runtime_sha256": sha256(executable), "runtime_arguments": runtime_args,
                 "fixture_manifest_sha256": sha256(manifest_path),
                 "save_file_sha256": sha256(output_save), "save_file_length": output_save.stat().st_size,
                 "save_slot": slot, "dispatch_instrumented": args.trace_dispatch,
                 "dispatch_events": events,
                 "stability_seconds": args.stability_seconds,
                 "observation": f"Autostart fixture needed no input; Save{slot:02d} copied after an unchanged interval and process termination."}
        if args.save_variant in {"save-boundaries", "load-boundaries"}:
            trace["intermediate_saves"] = {}
            for intermediate_slot in ((5,) if args.save_variant == "load-boundaries" else (1, 2)):
                intermediate = fixture / f"Save{intermediate_slot:02d}.lsd"
                if not intermediate.is_file():
                    raise RuntimeError(f"missing intermediate slot {intermediate_slot}")
                intermediate_copy = args.output.with_suffix(f".slot{intermediate_slot:02d}.lsd")
                intermediate_copy.write_bytes(intermediate.read_bytes())
                trace["intermediate_saves"][str(intermediate_slot)] = {
                    "sha256": sha256(intermediate_copy), "length": intermediate_copy.stat().st_size}
        if args.trace_save:
            trace["save_instrumented"] = True
        if args.trace_end_nest:
            trace["end_nest_instrumented"] = True
        if args.trace_opcode40:
            trace["opcode40_instrumented"] = True
        if args.trace_common1005:
            trace["common1005_instrumented"] = True
        if args.trace_common1005_frame:
            trace["common1005_frame_instrumented"] = True
        if args.trace_common1005_text:
            trace["common1005_text_instrumented"] = True
        if args.trace_end_command_battle:
            trace["end_command_battle_instrumented"] = True
        args.output.write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
        print(f"{args.output}: saved; {len(events)} native events", flush=True)
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        if session is not None:
            try:
                session.detach()
            except Exception:
                pass


if __name__ == "__main__":
    main()
