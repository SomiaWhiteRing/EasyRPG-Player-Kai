"""Capture registration call sites during fixed 250921 original JS startup."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import threading

import frida


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "build/maniac-original-js-host-symbols"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-registration-trace.json"
EXPECTED_EXE = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
AGENT = r"""
const base = Process.mainModule.base;
const ranges = [[0x17e030, 0x17e575], [0x1803e0, 0x180cfb], [0x181650, 0x18199c]];
for (const target of [0x139db0, 0x101790, 0xfac60]) {
    Interceptor.attach(base.add(target), {
        onEnter() {
            const site = this.returnAddress.sub(base).sub(5).toUInt32();
            if (ranges.some(([start, end]) => start <= site && site < end)) {
                send({target_rva: '0x' + target.toString(16),
                      call_site_rva: '0x' + site.toString(16)});
            }
        }
    });
}
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
            raise ValueError(f"fixture input changed: {name}")
    device = frida.get_local_device()
    pid = device.spawn([str(FIXTURE / "RPG_RT.exe")], cwd=str(FIXTURE))
    session = None
    events: list[dict[str, str]] = []
    errors: list[str] = []
    completed = threading.Event()
    try:
        session = device.attach(pid)
        script = session.create_script(AGENT)

        def on_message(message: dict, data: bytes | None) -> None:
            if message.get("type") == "send":
                payload = message["payload"]
                events.append(payload)
                if payload["call_site_rva"] == "0x17e4e3":
                    completed.set()
            else:
                errors.append(str(message))
                completed.set()

        script.on("message", on_message)
        script.load()
        device.resume(pid)
        if not completed.wait(10) or errors:
            raise RuntimeError(f"original registration did not finish: {errors}")
        audit = {
            "runtime_sha256": EXPECTED_EXE,
            "fixture_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
            "events": events,
            "scope": "Direct calls to three property/function registration helpers during one original 250921 startup, filtered to initializer and two host registration functions. Each RVA is stable across ASLR. The original process is stopped after the final picture registration; this trace does not establish callback behavior.",
        }
        OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
        print(f"{OUTPUT}: {len(events)} registration-helper calls captured")
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
