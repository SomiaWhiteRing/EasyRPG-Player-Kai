"""Observe original type-8 descriptor tables materialized by a fixed JS fixture."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import threading

import frida


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "build/maniac-original-js-host-symbols"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-lazy-registration-trace.json"
EXPECTED_EXE = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
AGENT = r"""
const base = Process.mainModule.base;
const events = [];
const active = [];
function rva(address) {
    const offset = address.sub(base).toInt32();
    return offset >= 0 && offset < Process.mainModule.size ? '0x' + offset.toString(16) : null;
}
Interceptor.attach(base.add(0x139bd0), {
    onEnter() {
        const descriptor = this.context.rsp.add(0x28).readPointer();
        this.output = this.context.rcx;
        this.record = {descriptor_rva:rva(descriptor), descriptor_type:descriptor.add(9).readU8(),
            table_rva:rva(descriptor.add(0x10).readPointer()),
            table_count:descriptor.add(0x18).readS32(),
            callback_rva:descriptor.add(9).readU8() === 0 ? rva(descriptor.add(0x18).readPointer()) : null,
            name_rva:rva(descriptor.readPointer()), callback_calls:[]};
        active.push(this.record);
    },
    onLeave() {
        this.record.object_payload = this.output.readPointer().toString();
        this.record.object_tag = this.output.add(8).readS32();
        events.push(this.record);
        if (active.pop() !== this.record) throw new Error('lazy registration call stack mismatch');
    }
});
Interceptor.attach(base.add(0x139db0), {onEnter() {
    const site = this.returnAddress.sub(base).sub(5).toInt32();
    if (site !== 0x139c81) return;
    if (active.length === 0) throw new Error('table registration without lazy creator');
    const descriptor = this.context.r9;
    active[active.length-1].callback_calls.push({call_site_rva:'0x139c81',
        descriptor_rva:rva(descriptor), name_rva:rva(descriptor.readPointer()),
        descriptor_type:descriptor.add(9).readU8(),
        callback_rva:descriptor.add(9).readU8() === 0 ? rva(descriptor.add(0x18).readPointer()) : null,
        object_payload:this.context.rdx.readPointer().toString(),
        object_tag:this.context.rdx.add(8).readS32()});
}});
Interceptor.attach(base.add(0x24f10), {onEnter() {send(events);}});
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    manifest_bytes = (FIXTURE / "manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest["save_variant"] != "js-host-symbols" or manifest["files"]["RPG_RT.exe"] != EXPECTED_EXE:
        raise ValueError("wrong fixed original JS fixture")
    for name, digest in manifest["files"].items():
        if sha256(FIXTURE / name) != digest:
            raise ValueError(f"fixture changed: {name}")
    device = frida.get_local_device()
    pid = device.spawn([str(FIXTURE / "RPG_RT.exe")], cwd=str(FIXTURE))
    session = None
    completed = threading.Event()
    payloads: list[list[dict]] = []
    errors: list[str] = []
    try:
        session = device.attach(pid)
        script = session.create_script(AGENT)

        def on_message(message: dict, _data: bytes | None) -> None:
            if message.get("type") == "send":
                payloads.append(message["payload"])
                completed.set()
            else:
                errors.append(str(message))
                completed.set()

        script.on("message", on_message)
        script.load()
        device.resume(pid)
        if not completed.wait(20) or errors or not payloads:
            raise RuntimeError(f"lazy registration probe failed: {errors}")
        trace = {"runtime_sha256": EXPECTED_EXE,
                 "fixture_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
                 "events": payloads[0],
                 "scope": "Read-only type-8 auto-initializer and descriptor-loop hooks through first Maniac save in the existing original js-host-symbols fixture. No extra JavaScript is evaluated by the probe."}
        OUTPUT.write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
        print(f"{OUTPUT}: {len(payloads[0])} lazy objects observed")
    finally:
        try:
            device.kill(pid)
        except frida.InvalidOperationError:
            pass
        if session is not None:
            try:
                session.detach()
            except frida.InvalidOperationError:
                pass


if __name__ == "__main__":
    main()
