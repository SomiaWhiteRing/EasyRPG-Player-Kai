"""Observe the original 250921 End Command branch in fixed map and battle fixtures."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]
REFERENCE_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
FIXTURES = (
    ("map-autostart", ROOT / "build/maniac-original-js-host-negative", "js-host-negative"),
    ("map-parallel-common", ROOT / "build/maniac-250921-noop-long-skip", "noop-long-skip"),
    ("map-autostart-single", ROOT / "build/maniac-original-js-exception-continue", "js-exception-continue"),
)
OUTPUT = ROOT / "docs/evidence/maniac-250921-00000-end-command-trace.json"
AGENT = r"""
const base = Process.mainModule.base;
let records = 0;
function snapshot(stage, owner) {
    const depth = owner.readS32();
    const mode = owner.add(0x38).readU8();
    let cursor = null, opcode = null, indent = null;
    if (depth > 0 && depth < 64) {
        const frame = owner.add(8).readPointer().add((depth - 1) * 8).readPointer();
        cursor = frame.add(0x24).readS32();
        const list = frame.readPointer().readPointer();
        if (cursor > 0 && cursor <= list.readS32()) {
            const command = list.add(8).readPointer().add((cursor - 1) * 8).readPointer();
            opcode = command.readS32();
            indent = command.add(4).readS32();
        }
    }
    send({stage, owner_mode: mode, frame_depth: depth, cursor, opcode, indent});
}
Interceptor.attach(base.add(0x4762f), {onEnter(args) {
    if (records++ < 12) snapshot("entry", this.context.r13);
}});
Interceptor.attach(base.add(0x47642), {onEnter(args) {
    if (records < 12) snapshot("single_frame_reset", this.context.r13);
}});
Interceptor.attach(base.add(0x47662), {onEnter(args) {
    if (records < 12) snapshot("frame_cleanup", this.context.r13);
}});
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(label: str, fixture: Path, variant: str) -> dict:
    manifest_path = fixture / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    if manifest["save_variant"] != variant or manifest["files"]["RPG_RT.exe"] != REFERENCE_SHA256:
        raise ValueError(f"wrong fixture: {label}")
    for name, digest in manifest["files"].items():
        if sha256(fixture / name) != digest:
            raise ValueError(f"changed fixture: {label}/{name}")
    for saved in fixture.glob("Save*.lsd"):
        if saved.name not in manifest["files"]:
            saved.unlink()
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(fixture / "RPG_RT.exe")], cwd=fixture,
                               startupinfo=startup, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    events: list[dict] = []
    errors: list[dict] = []
    session = None
    try:
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
        deadline = time.monotonic() + 14
        while time.monotonic() < deadline and not any(e["stage"] == "entry" for e in events):
            if process.poll() is not None:
                raise RuntimeError(f"{label}: original runtime exited before End Command")
            time.sleep(0.1)
        if not any(e["stage"] == "entry" for e in events):
            raise RuntimeError(f"{label}: no End Command within 14 seconds")
        time.sleep(8 if label == "map-parallel-common" else 0.3)
        if errors:
            raise RuntimeError(f"{label}: native hook errors: {errors}")
        for event in events:
            if event["opcode"] != 0:
                raise ValueError(f"{label}: target block did not execute opcode 0: {event}")
        return {"context": label, "fixture_manifest_sha256": sha256(manifest_path),
                "events": events}
    finally:
        if process.poll() is None:
            process.kill()
            process.wait()
        if session is not None:
            try:
                session.detach()
            except Exception:
                pass


def main() -> None:
    records = [capture(label, fixture, variant) for label, fixture, variant in FIXTURES]
    output = {"reference_sha256": REFERENCE_SHA256, "fixtures": records,
              "observation": "Read-only native hooks at End Command branch entry and its reset/cleanup blocks. Original processes terminated by collector after observation."}
    OUTPUT.write_text(json.dumps(output, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: " + ", ".join(f"{r['context']}={len(r['events'])}" for r in records))


if __name__ == "__main__":
    main()
