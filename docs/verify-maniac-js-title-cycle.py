"""Verify 250921 JavaScript context reuse and title/new-game reset evidence."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
MANIFEST = EVIDENCE / "maniac-250921-js-title-cycle-manifest.json"
TRACE = EVIDENCE / "maniac-250921-js-title-cycle-trace.json"
OUTPUT = EVIDENCE / "maniac-250921-js-title-cycle-audit.json"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
RUNTIME_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
MAP_SHA256 = "8f19ed568c044aeeddfa52694510f4be3c58b1c7069eca1c701f9a2c5d991a9f"


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def stage(sample: dict) -> str:
    length, slots = sample["vector_length"], sample["slots"]
    if length == 1 and slots == [0, None, None, None, None]:
        return "new_game_initial"
    if length == 4 and slots == [0, 0, 0, 0, None]:
        return "first_script_reads_zero"
    if length == 5 and slots == [0, 0, 0, 0, 42]:
        return "later_script_reads_42"
    raise ValueError(f"unexpected JavaScript title-cycle sample: {sample}")


def instruction(image: pefile.PE, decoder: Cs, rva: int) -> str:
    base = image.OPTIONAL_HEADER.ImageBase
    decoded = list(decoder.disasm(image.get_data(rva, 16), base + rva, count=1))
    if len(decoded) != 1:
        raise ValueError(f"no instruction at {rva:#x}")
    return f"{decoded[0].mnemonic} {decoded[0].op_str}"


def main() -> None:
    manifest_bytes = MANIFEST.read_bytes()
    manifest = json.loads(manifest_bytes)
    trace = json.loads(TRACE.read_text(encoding="utf-8-sig"))
    if (manifest["save_variant"] != "js-title-cycle" or
            manifest["files"]["RPG_RT.exe"] != RUNTIME_SHA256 or
            manifest["files"]["Map0001.lmu"] != MAP_SHA256 or
            manifest["expected_global_slots_after_event"] != [0, 0, 0, 0, 42] or
            manifest["expected_global_numeric_vector_length_after_event"] != 5 or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            trace["reference_sha256"] != RUNTIME_SHA256 or
            sha256(RUNTIME.read_bytes()) != RUNTIME_SHA256):
        raise ValueError("JavaScript title-cycle input is not the frozen original fixture")
    samples = trace["samples"]
    if len(samples) != 45 or [sample["input_enter_count"] for sample in samples] != list(range(45)):
        raise ValueError("JavaScript title-cycle trace is incomplete")
    if any(sample["global_switch_vector_length"] != 1 or
           sample["global_switch_slots_raw"] != [0] for sample in samples):
        raise ValueError("unexpected global switch state")
    stages = [stage(sample) for sample in samples]
    cycle = ["new_game_initial", "first_script_reads_zero", "later_script_reads_42"]
    if stages[:9] != cycle * 3:
        raise ValueError(f"three complete title/new-game cycles not observed: {stages[:9]}")

    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    expected_instructions = {
        0x596E0: "call 0x140181650",  # initial runtime startup
        0x47D38: "call 0x140027d00",  # 12510 Return to Title handler
        0x27D10: "call 0x140053c10",  # throw title-control exception
        0x1FCB35: "call 0x140181650",  # title/new-game control catch
        0x181675: "call 0x1400f6080",  # free existing context
        0x18167A: "mov qword ptr [rip + 0xf7917], rsi",  # clear context pointer
        0x18168D: "call 0x1400f5830",  # free existing runtime
        0x181692: "mov qword ptr [rip + 0xf78f7], rsi",  # clear runtime pointer
        0x1816DB: "call 0x1400f5190",  # create replacement runtime
        0x1816F3: "call 0x1400f5b00",  # create replacement context
    }
    observed = {f"0x{rva:x}": instruction(image, decoder, rva)
                for rva in expected_instructions}
    if any(observed[f"0x{rva:x}"] != expected for rva, expected in expected_instructions.items()):
        raise ValueError(f"JavaScript lifecycle calls changed: {observed}")
    transitions = []
    for sample, current in zip(samples, stages):
        if not transitions or transitions[-1]["stage"] != current:
            transitions.append({"input_enter_count": sample["input_enter_count"], "stage": current})
    audit = {
        "runtime_sha256": RUNTIME_SHA256,
        "manifest_sha256": sha256(manifest_bytes),
        "trace_sha256": sha256(TRACE.read_bytes()),
        "map_sha256": MAP_SHA256,
        "sample_count": len(samples),
        "verified_complete_cycles": 3,
        "transitions": transitions,
        "static_call_sites": observed,
        "scope": "250921 Windows original 3030 globalThis property persists between successive commands in one new game, and is absent at the first command after 12510 Return to Title/New Game. The 12510 handler reaches the title-control exception, whose catch calls the same initializer as startup; that initializer frees and clears prior context/runtime before replacing them. Save/load and shutdown boundaries remain separate.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(samples)} samples, three complete cycles")


if __name__ == "__main__":
    main()
