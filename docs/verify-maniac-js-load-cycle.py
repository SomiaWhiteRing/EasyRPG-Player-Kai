"""Verify original 250921 JavaScript state and event continuation across 3002/3003."""

from __future__ import annotations

import hashlib
import json
import runpy
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-js-load-cycle-manifest.json"
TRACE = EVIDENCE / "maniac-250921-js-load-cycle-trace.json"
SAVE = EVIDENCE / "maniac-250921-js-load-cycle-save.lsd"
OUTPUT = EVIDENCE / "maniac-250921-js-load-cycle-audit.json"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
RUNTIME_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
MAP_SHA256 = "3f719b244677ee458c080ed55a511705c1b8c0ab0efe3b34d13d1def5329842d"


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def stage(sample: dict) -> str:
    key = sample["vector_length"], tuple(sample["slots"])
    states = {
        (1, (0, None, None, None, None)): "initial",
        (4, (0, 0, 0, 42, None)): "before_load_script_42",
        (5, (0, 0, 0, 42, 43)): "before_load_script_43",
        (4, (0, 0, 0, 99, None)): "after_load_script_undefined",
        (5, (0, 0, 0, 99, 43)): "after_load_script_43",
    }
    if key not in states:
        raise ValueError(f"unexpected JavaScript load-cycle state: {sample}")
    return states[key]


def main() -> None:
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    ber, chunks = parser["ber"], parser["chunks"]
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace = json.loads(TRACE.read_text(encoding="utf-8-sig"))
    save_bytes = SAVE.read_bytes()
    if (manifest["save_variant"] != "js-load-cycle" or manifest["expected_save_slot"] != 1 or
            manifest["files"]["RPG_RT.exe"] != RUNTIME_SHA256 or
            manifest["files"]["Map0001.lmu"] != MAP_SHA256 or
            manifest["expected_global_slots_after_event"] != [0, 0, 0, 42, 43] or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["reference_sha256"] != RUNTIME_SHA256 or
            trace["save_file_sha256"] != sha256(save_bytes) or
            sha256(RUNTIME.read_bytes()) != RUNTIME_SHA256):
        raise ValueError("JavaScript load-cycle fixture, trace, save, or runtime changed")
    header_length, position = ber(save_bytes, 0)
    if save_bytes[position:position + header_length] != b"LcfSaveData":
        raise ValueError("original save header changed")
    root = chunks(save_bytes, position + header_length, len(save_bytes))
    if not any(entry["id"] == 101 for entry in root):
        raise ValueError("original save lacks system state")
    samples = trace["samples"]
    if len(samples) != 45 or [sample["input_enter_count"] for sample in samples] != list(range(45)):
        raise ValueError("JavaScript load-cycle trace is incomplete")
    if any(sample["global_switch_vector_length"] != 1 or
           sample["global_switch_slots_raw"] != [0] for sample in samples):
        raise ValueError("unexpected global switch state")
    stages = [stage(sample) for sample in samples]
    expected_prefix = ["initial", "before_load_script_42", "before_load_script_42",
                       "before_load_script_43", "initial", "after_load_script_undefined",
                       "after_load_script_undefined", "after_load_script_43"]
    if stages[:8] != expected_prefix:
        raise ValueError(f"save/load behavior or timing changed: {stages[:8]}")
    if "before_load_script_42" in stages[4:] or "before_load_script_43" in stages[4:]:
        raise ValueError("pre-load JavaScript context unexpectedly returned")

    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    base = image.OPTIONAL_HEADER.ImageBase
    call = next(decoder.disasm(image.get_data(0x1FCB35, 16), base + 0x1FCB35, count=1))
    if f"{call.mnemonic} {call.op_str}" != "call 0x140181650":
        raise ValueError("load control catch no longer reaches JavaScript initializer")
    transitions = []
    for sample, current in zip(samples, stages):
        if not transitions or transitions[-1]["stage"] != current:
            transitions.append({"input_enter_count": sample["input_enter_count"], "stage": current})
    audit = {
        "runtime_sha256": RUNTIME_SHA256,
        "manifest_sha256": sha256(manifest_bytes),
        "trace_sha256": sha256(TRACE.read_bytes()),
        "save_sha256": sha256(save_bytes),
        "map_sha256": MAP_SHA256,
        "sample_count": len(samples),
        "transitions": transitions,
        "load_reset_call_rva": "0x1fcb35",
        "scope": "250921 Windows original 3002 saves while JS global property is 42; subsequent 3030 reads 42, mutation to 43 is observed, and 3003 load resets the numeric vector and JS context. Resumed event commands read undefined as sentinel 99 before reassigning 43. This is one valid save slot and one auto event; other save modes remain separate.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(samples)} samples, save/load context reset observed")


if __name__ == "__main__":
    main()
