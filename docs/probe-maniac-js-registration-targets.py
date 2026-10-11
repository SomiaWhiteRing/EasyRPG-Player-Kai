"""Capture target JS object identities for original startup property helpers."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import threading

import frida


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "build/maniac-original-js-host-symbols"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-registration-targets-trace.json"
EXPECTED_EXE = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
AGENT = r"""
const base = Process.mainModule.base;
const events = [];
const functions = [];
const objects = [];
let context = null;
function valueAt(address) {
    return {object_payload: address.readPointer().toString(), object_tag: address.add(8).readS32()};
}
function rva(address) {
    const offset = address.sub(base).toInt32();
    return offset >= 0 && offset < Process.mainModule.size ? '0x' + offset.toString(16) : null;
}
Interceptor.attach(base.add(0xfac60), {
    onEnter() {
        this.output = this.context.rcx;
        context = this.context.rdx;
        const name = this.context.r9;
        this.record = {call_site_rva:'0x' + this.returnAddress.sub(base).sub(5).toInt32().toString(16),
                       callback_rva:'0x' + this.context.r8.sub(base).toInt32().toString(16),
                       name: name.isNull() ? null : name.readUtf8String()};
    },
    onLeave() {functions.push({...this.record, ...valueAt(this.output)});}
});
Interceptor.attach(base.add(0xfa6f0), {
    onEnter() {
        this.site = this.returnAddress.sub(base).sub(5).toInt32();
        this.output = this.context.rcx;
        this.prototype = valueAt(this.context.r8);
        this.class_id = this.context.r9.toUInt32();
    },
    onLeave() {
        if (this.site === 0x17b291) objects.push({call_site_rva:'0x17b291',
            class_id:this.class_id, prototype:this.prototype, ...valueAt(this.output)});
    }
});
for (const target of [0x139db0, 0x101790]) {
    Interceptor.attach(base.add(target), {onEnter() {
        const site = this.returnAddress.sub(base).sub(5).toInt32();
        if (site < 0 || site >= Process.mainModule.size) return;
        const value = this.context.rdx;
        context = this.context.rcx;
        const event = {
            helper_rva: '0x' + target.toString(16),
            call_site_rva: '0x' + site.toString(16),
            object_payload: value.readPointer().toString(),
            object_tag: value.add(8).readS32()
        };
        if (target === 0x139db0) {
            const descriptor = this.context.r9;
            const name = descriptor.readPointer();
            event.descriptor_rva = '0x' + descriptor.sub(base).toInt32().toString(16);
            event.name_rva = name.isNull() ? null : '0x' + name.sub(base).toInt32().toString(16);
            event.descriptor_bytes = Array.from(new Uint8Array(descriptor.readByteArray(32)));
            event.descriptor_value_10_rva = rva(descriptor.add(0x10).readPointer());
            event.descriptor_value_18_rva = rva(descriptor.add(0x18).readPointer());
        } else {
            event.atom = this.context.r8.toUInt32();
        }
        events.push(event);
    }});
}
Interceptor.attach(base.add(0x17e556), {onEnter() {
    const runtime = context.add(0x20).readPointer();
    const count = runtime.add(0x6c).readS32();
    if (count < 1 || count > 256) throw new Error('invalid class prototype count');
    const prototypes = context.add(0x48).readPointer();
    send({events, functions, objects, image_base:base.toString(), global: valueAt(context.add(0x170)),
          iterator_prototype:valueAt(context.add(0x120)),
          async_iterator_prototype:valueAt(context.add(0x130)),
          class_prototypes:Array.from({length:count}, (_,i)=>({index:i,...valueAt(prototypes.add(i*16))}))});
}});
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
    payloads: list[dict] = []
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
        if not completed.wait(12) or errors or not payloads:
            raise RuntimeError(f"startup target-object probe failed: {errors}")
        trace = {"runtime_sha256": EXPECTED_EXE,
                 "fixture_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
                 **payloads[0],
                 "scope": "Read-only JSValue payload/tag identities at every named-descriptor and atom-property helper call before picture registration in one fixed original startup. Payloads are process-local identity tokens, not portable addresses."}
        OUTPUT.write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
        print(f"{OUTPUT}: {len(payloads[0]['events'])} target-object observations, {len(payloads[0]['functions'])} native function results")
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
