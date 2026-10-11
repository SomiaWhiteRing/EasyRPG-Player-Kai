"""Capture one fixed 250921 JavaScript getter fault before dismissing its native dialog."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from threading import Event


ROOT = Path(__file__).resolve().parents[1]
REFERENCE_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
AGENT = r"""
const base = Process.mainModule.base;
function variables() {
    const begin = base.add(0x27a4b8).readPointer();
    const end = base.add(0x27a4c0).readPointer();
    const count = end.sub(begin).toInt32() / 4;
    return Array.from({length: Math.max(0, Math.min(count, 6))}, (_, i) => begin.add(i * 4).readS32());
}
Interceptor.attach(Process.getModuleByName("user32.dll").getExportByName("MessageBoxW"), {
    onEnter(args) {
        send({api: "MessageBoxW", message: args[1].readUtf16String(),
              caption: args[2].readUtf16String(), numeric_variables: variables()});
    }
});
"""


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    fixture = args.fixture.resolve()
    manifest_path = fixture / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    subtype = manifest.get("js_edge_case")
    if manifest.get("save_variant") != "js-host-edges" or subtype not in ("number", "switch", "string"):
        raise ValueError("requires a selected original JS host fault fixture")
    if manifest["files"].get("RPG_RT.exe") != REFERENCE_SHA256:
        raise ValueError("wrong original runtime")
    for name, digest in manifest["files"].items():
        if sha256(fixture / name) != digest:
            raise ValueError(f"fixture input changed: {name}")
    for saved in fixture.glob("Save*.lsd"):
        if saved.name not in manifest["files"]:
            saved.unlink()
    executable = fixture / "RPG_RT.exe"
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([str(executable)], cwd=fixture, startupinfo=startup,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    observed = Event()
    messages: list[dict] = []
    errors: list[dict] = []
    session = None
    try:
        import frida
        session = frida.attach(process.pid)
        script = session.create_script(AGENT)

        def on_message(message, _data):
            if message.get("type") == "send":
                messages.append(message["payload"])
            else:
                errors.append(message)
            observed.set()

        script.on("message", on_message)
        script.load()
        if not observed.wait(25):
            raise RuntimeError("original runtime did not show a native fault within 25 seconds")
        if errors:
            raise RuntimeError(f"native fault hook failed: {errors}")
        if len(messages) != 1:
            raise RuntimeError(f"expected one native fault dialog, saw {messages}")
        if process.poll() is not None:
            raise RuntimeError(f"original runtime exited before fault capture: {process.returncode}")
        dialog = messages[0]
        if not any(word in dialog["message"] for word in ("Invalid", "Access", "Error")):
            raise RuntimeError(f"unexpected dialog: {dialog}")
        args.output.parent.mkdir(parents=True, exist_ok=True)
        record = {
            "reference_sha256": REFERENCE_SHA256,
            "runtime_sha256": sha256(executable),
            "runtime_arguments": [],
            "fixture_manifest_sha256": sha256(manifest_path),
            "selected_case": f"{subtype}/error/id_proxy_get",
            "native_dialog": dialog,
            "observation": "Original runtime reached MessageBoxW; collector terminated the process with the dialog pending. No normal continuation or save is asserted."
        }
        args.output.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print(f"{args.output}: {json.dumps(dialog, ensure_ascii=False)}", flush=True)
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
