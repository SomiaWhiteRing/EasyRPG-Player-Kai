"""Bind one live 250921 window-close observation to the JS shutdown instructions."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
TRACE = EVIDENCE / "maniac-250921-js-shutdown-trace.json"
MANIFEST = EVIDENCE / "maniac-250921-js-title-cycle-manifest.json"
OUTPUT = EVIDENCE / "maniac-250921-js-shutdown-audit.json"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
EXPECTED_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def main() -> None:
    trace_bytes = TRACE.read_bytes()
    trace = json.loads(trace_bytes)
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    if (sha256(RUNTIME.read_bytes()) != EXPECTED_SHA256 or
            trace["runtime_sha256"] != EXPECTED_SHA256 or
            manifest["files"]["RPG_RT.exe"] != EXPECTED_SHA256 or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes)):
        raise ValueError("shutdown trace is not bound to the fixed original runtime and fixture")
    events = trace["events"]
    if [event["event"] for event in events] != ["attached", "shutdown_enter", "shutdown_leave"]:
        raise ValueError("shutdown was skipped or repeated")
    if any(event["context"] == "0x0" or event["runtime"] == "0x0" for event in events[:2]):
        raise ValueError("JavaScript pointers were not live before shutdown")
    if events[2]["context"] != "0x0" or events[2]["runtime"] != "0x0":
        raise ValueError("JavaScript pointers were not both cleared on shutdown return")
    if not isinstance(trace["exit_code"], int):
        raise ValueError("original process exit code was not recorded")

    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    base = image.OPTIONAL_HEADER.ImageBase
    expected = {
        0x1837A2: "call 0x140059cf0",
        0x5A495: "mov rcx, qword ptr [rip + 0x21eafc]",
        0x5A4A1: "call 0x1400f6080",
        0x5A4A6: "mov qword ptr [rip + 0x21eaeb], rbp",
        0x5A4AD: "mov rcx, qword ptr [rip + 0x21eadc]",
        0x5A4B9: "call 0x1400f5830",
        0x5A4BE: "mov qword ptr [rip + 0x21eacb], rbp",
    }
    observed = {}
    for rva, instruction in expected.items():
        decoded = list(decoder.disasm(image.get_data(rva, 16), base + rva, count=1))
        if len(decoded) != 1:
            raise ValueError(f"missing shutdown instruction at {rva:#x}")
        actual = f"{decoded[0].mnemonic} {decoded[0].op_str}"
        if actual != instruction:
            raise ValueError(f"shutdown instruction changed at {rva:#x}: {actual}")
        observed[f"0x{rva:x}"] = actual
    audit = {
        "runtime_sha256": EXPECTED_SHA256,
        "fixture_manifest_sha256": sha256(manifest_bytes),
        "live_trace_sha256": sha256(trace_bytes),
        "window_close_exit_code": trace["exit_code"],
        "shutdown_enter_count": 1,
        "shutdown_leave_count": 1,
        "live_pointer_state": [
            {"event": event["event"], "context_nonnull": event["context"] != "0x0",
             "runtime_nonnull": event["runtime"] != "0x0"} for event in events],
        "static_instruction_sites": observed,
        "scope": "One 250921 Windows WM_CLOSE execution enters and returns from the shutdown routine once with live JS context/runtime pointers becoming null. Static call sites show context release then runtime release and pointer clears. The process exit code is recorded without treating it as success; abnormal-exit and forced-kill paths remain outside this result.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: one JS shutdown observed and static release order verified")


if __name__ == "__main__":
    main()
