"""Inventory registration-helper callers across fixed original JS startup."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import threading

import frida


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "build/maniac-original-js-host-symbols"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-registration-closure-trace.json"
EXPECTED_EXE = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
AGENT = r"""
const base = Process.mainModule.base;
const records = new Map();
const helpers = [0x139db0,0x101790,0xfac60];
function rva(address) {
    if (address.isNull()) return null;
    const value = address.sub(base).toInt32();
    return value >= 0 && value < Process.mainModule.size ? '0x' + value.toString(16) : null;
}
for (const target of helpers) {
    Interceptor.attach(base.add(target), {onEnter() {
        const site = this.returnAddress.sub(base).sub(5).toInt32();
        if (site < 0 || site >= Process.mainModule.size) return;
        const key = target.toString(16)+':'+site.toString(16);
        const prior = records.get(key);
        if (prior !== undefined) {prior.count++; return;}
        const result = {target_rva:'0x'+target.toString(16),call_site_rva:'0x'+site.toString(16),count:1};
        if (target === 0x139db0) {
            const d = this.context.r9;
            result.descriptor_rva = rva(d);
            result.descriptor_type = d.add(9).readU8();
            result.descriptor_flags = d.add(8).readU8();
            result.name_rva = rva(d.readPointer());
            result.descriptor_value_10_rva = rva(d.add(0x10).readPointer());
            result.descriptor_value_18_rva = rva(d.add(0x18).readPointer());
            result.callback_rva = result.descriptor_type === 0 ? result.descriptor_value_18_rva : null;
            if (result.name_rva !== null) {
                try {result.name = d.readPointer().readUtf8String(80);} catch(e) {}
            }
            result.object_payload = this.context.rdx.readPointer().toString();
            result.object_tag = this.context.rdx.add(8).readS32();
        } else if (target === 0xfac60) {
            result.callback_rva = rva(this.context.r8);
            result.name_rva = rva(this.context.r9);
        }
        records.set(key,result);
    }});
}
Interceptor.attach(base.add(0x17e50b), {onEnter() {
    send(Array.from(records.values()).sort((a,b)=>parseInt(a.call_site_rva)-parseInt(b.call_site_rva)));
}});
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    manifest_bytes = (FIXTURE / "manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest["save_variant"] != "js-host-symbols" or manifest["files"]["RPG_RT.exe"] != EXPECTED_EXE:
        raise ValueError("wrong original JS fixture")
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
        if not completed.wait(12) or errors or not payloads:
            raise RuntimeError(f"startup registration inventory failed: {errors}")
        audit = {"runtime_sha256": EXPECTED_EXE,
                 "fixture_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
                 "events": payloads[0],
                 "scope": "Read-only helper call-site and descriptor inventory through picture registration. Enumerates all callers reached in this fixed startup, including calls outside direct host initializers; unexecuted branches require separate static review."}
        OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
        print(f"{OUTPUT}: {len(payloads[0])} distinct startup registration sites")
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
