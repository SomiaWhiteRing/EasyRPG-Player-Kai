"""Observe the fixed 250921 runtime's JavaScript pointers during a normal window close."""

from __future__ import annotations

import ctypes
import hashlib
import json
from pathlib import Path
import subprocess
import time

import frida


ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "build/maniac-original-js-title-cycle"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-shutdown-trace.json"
EXPECTED_EXE = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
AGENT = r"""
const base = Process.mainModule.base;
const contextSlot = base.add(0x278f98);
const runtimeSlot = base.add(0x278f90);
function snapshot(name) {
    send({event: name, context: contextSlot.readPointer().toString(),
          runtime: runtimeSlot.readPointer().toString()});
}
snapshot('attached');
Interceptor.attach(base.add(0x59cf0), {
    onEnter() { snapshot('shutdown_enter'); },
    onLeave() { snapshot('shutdown_leave'); }
});
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def windows_for_pid(pid: int) -> list[int]:
    user32 = ctypes.windll.user32
    handles: list[int] = []
    callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

    def visit(hwnd: int, unused: int) -> bool:
        owner = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(ctypes.c_void_p(hwnd)):
            handles.append(hwnd)
        return True

    callback = callback_type(visit)
    user32.EnumWindows(callback, 0)
    return handles


def main() -> None:
    manifest_bytes = (FIXTURE / "manifest.json").read_bytes()
    manifest = json.loads(manifest_bytes)
    if manifest["save_variant"] != "js-title-cycle" or manifest["files"]["RPG_RT.exe"] != EXPECTED_EXE:
        raise ValueError("wrong fixed fixture")
    for name, digest in manifest["files"].items():
        if sha256(FIXTURE / name) != digest:
            raise ValueError(f"fixture file changed: {name}")
    events: list[dict[str, str]] = []
    errors: list[str] = []
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0  # SW_HIDE
    process = subprocess.Popen([str(FIXTURE / "RPG_RT.exe")], cwd=FIXTURE,
                               startupinfo=startup, stdout=subprocess.DEVNULL,
                               stderr=subprocess.DEVNULL)
    session = None
    try:
        deadline = time.monotonic() + 10
        while not windows_for_pid(process.pid):
            if process.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("original game window did not open")
            time.sleep(0.1)
        session = frida.attach(process.pid)
        script = session.create_script(AGENT)

        def on_message(message: dict, data: bytes | None) -> None:
            if message.get("type") == "send":
                events.append(message["payload"])
            else:
                errors.append(str(message))

        script.on("message", on_message)
        script.load()
        if not events or events[0]["event"] != "attached":
            raise RuntimeError(f"Frida agent did not attach: {errors}")
        handles = windows_for_pid(process.pid)
        if len(handles) != 1 or not ctypes.windll.user32.PostMessageW(ctypes.c_void_p(handles[0]), 0x10, 0, 0):
            raise RuntimeError(f"could not close one original game window: {handles}")
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError("original runtime did not exit after WM_CLOSE") from exc
        if errors:
            raise RuntimeError(f"Frida agent errors: {errors}")
        names = [event["event"] for event in events]
        required = ["attached", "shutdown_enter", "shutdown_leave"]
        if names != required:
            raise ValueError(f"unexpected shutdown hook order or repeated release: {names}")
        if events[1]["context"] == "0x0" or events[1]["runtime"] == "0x0":
            raise ValueError("JavaScript context/runtime were not live before release")
        if events[2]["context"] != "0x0" or events[2]["runtime"] != "0x0":
            raise ValueError("both JavaScript pointers were not cleared")
        audit = {
            "runtime_sha256": EXPECTED_EXE,
            "fixture_manifest_sha256": hashlib.sha256(manifest_bytes).hexdigest(),
            "exit_code": process.returncode,
            "events": events,
            "scope": "One WM_CLOSE on original 250921 Windows RPG_RT.exe enters and returns from shutdown once; live non-null JavaScript context and runtime pointers are both cleared on return. Static instructions establish their release order. The observed process exit code is recorded separately; crash and forced-termination paths are not inferred.",
        }
        OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
        print(f"{OUTPUT}: one window-close shutdown cleared both JavaScript pointers")
    finally:
        if session is not None:
            try:
                session.detach()
            except frida.InvalidOperationError:
                pass
        if process.poll() is None:
            process.kill()
            process.wait()


if __name__ == "__main__":
    main()
