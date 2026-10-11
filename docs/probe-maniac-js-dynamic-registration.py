"""Read every dynamic-name native function created during fixed original JS startup."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import threading

import frida


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "build/maniac-original-js-host-symbols"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-dynamic-registration-trace.json"
EXPECTED_EXE = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SITES = (0x139FF6, 0x13A077, 0x15FE10, 0x170358, 0x1703A0, 0x17B6D3)
AGENT = r"""
const base = Process.mainModule.base;
const sites = new Set([0x139ff6,0x13a077,0x15fe10,0x170358,0x1703a0,0x17b6d3]);
const events = [];
Interceptor.attach(base.add(0xfac60), {onEnter() {
    const site = this.returnAddress.sub(base).sub(5).toInt32();
    if (!sites.has(site)) return;
    const name = this.context.r9;
    const callback = this.context.r8.sub(base).toInt32();
    let nameBytes = null;
    let error = null;
    if (!name.isNull()) {
        try {
            nameBytes = Array.from(new Uint8Array(name.readByteArray(96)));
        } catch (e) {error = String(e);}
    }
    events.push({call_site_rva:'0x'+site.toString(16), callback_rva:'0x'+callback.toString(16),
                 name_is_null:name.isNull(), name_bytes:nameBytes, read_error:error,
                 context_ptr:this.context.rdx.toString()});
}});
Interceptor.attach(base.add(0x17e50b), {onEnter() {send(events);}});
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    manifest_bytes = (FIXTURE / "manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest["save_variant"] != "js-host-symbols" or manifest["files"]["RPG_RT.exe"] != EXPECTED_EXE:
        raise ValueError("wrong fixed original fixture")
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
            raise RuntimeError(f"dynamic registration probe failed: {errors}")
        events = payloads[0]
        for event in events:
            raw = event.pop("name_bytes")
            if event["read_error"]:
                raise RuntimeError(f"could not read dynamic name: {event}")
            if raw is None:
                event["name"] = None
            else:
                blob = bytes(raw)
                if b"\0" not in blob:
                    raise ValueError(f"dynamic name exceeds 96 bytes: {event}")
                event["name"] = blob.split(b"\0", 1)[0].decode("utf-8", errors="replace")
        if {int(e["call_site_rva"], 16) for e in events} != set(SITES):
            raise ValueError("a fixed dynamic registration site was not observed")
        trace = {"runtime_sha256": EXPECTED_EXE,
                 "fixture_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
                 "events": events,
                 "scope": "All calls from six generic dynamic-name native function creation sites before picture registration in one original startup. Names are read from the actual r9 pointer at helper entry; null is preserved. No Player behavior or unexecuted branch is inferred."}
        OUTPUT.write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
        print(f"{OUTPUT}: {len(events)} dynamic-name calls across six sites")
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
