"""Bind opcode 1006's 250921 consumer to explicit selector and battle outcomes."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-1006-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCE_HASHES = {
    "commands/01006.md": "416b0e46611a2e5ab50574cb2081cf096beff2945a808fe7d7a5ed534db87077",
    "evidence/runtime-250921/0001d2b0.c": "4e2e0b9d1d89a2659514c30beef0165b418d239fa51d60f0036861500516ea38",
    "evidence/native-250921/0004bb60.c": "58afceba30a0742503a02ba7a79283e597df269c0841dcc0dda72c90c3331dbf",
}
SITES = {
    0x46A60: "call 0x1d2b0", 0x46A65: "mov al, 1",
    0x1D2C5: "cmp edx, 3", 0x1D2D8: "cmp edx, 2",
    0x1D2F6: "and ecx, 0xf", 0x1D2FF: "cmp ecx, 1",
    0x1D317: "call 0x17d7c0", 0x1D32E: "cmovs ecx, eax",
    0x1D361: "call 0x17d7c0", 0x1D38F: "call 0x17d7c0",
    0x1D3C6: "cmp byte ptr [rip + 0x25bd64], bl", 0x1D3CC: "je 0x1d52c",
    0x1D3D2: "test edi, edi", 0x1D3DA: "sub edi, 1", 0x1D3DF: "cmp edi, 1",
    0x1D3E8: "cmp dword ptr [rip + 0x25bcbd], 3",
    0x1D3FE: "jbe 0x1d53c", 0x1D40F: "cmp byte ptr [rcx + 0xe8], bl",
    0x1D41B: "cmp dword ptr [rcx + 0x20], ebx", 0x1D433: "jmp 0x18eb90",
    0x1D438: "test sil, 1", 0x1D43E: "cmp dword ptr [rip + 0x25bc67], 3",
    0x1D452: "mov dword ptr [rip + 0x25bc24], 0x64", 0x1D483: "call 0x18eb90",
    0x1D4A0: "test sil, 1", 0x1D4A6: "cmp dword ptr [rip + 0x25bbff], 4",
    0x1D4C5: "mov dword ptr [rip + 0x25bbb1], 0x64",
    0x1D4CF: "mov byte ptr [rip + 0x25bc6f], bl",
    0x1D4FF: "call 0x924a0", 0x1D50B: "call 0x18eb90",
    0x1D545: "call 0xc8fc0",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    if digest(RUNTIME) != RUNTIME_SHA:
        raise ValueError("fixed original changed")
    for name, expected in SOURCE_HASHES.items():
        if digest(SOURCE / name) != expected:
            raise ValueError(f"reviewed source changed: {name}")
    pe = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    checked = {}
    for rva, expected in SITES.items():
        instruction = next(decoder.disasm(pe.get_data(rva, 16), rva))
        text = f"{instruction.mnemonic} {instruction.op_str}"
        if text != expected:
            raise ValueError(f"changed instruction at {rva:#x}: {text}")
        checked[f"0x{rva:x}"] = {"instruction": text, "bytes": instruction.bytes.hex()}
    audit = {
        "runtime_sha256": RUNTIME_SHA,
        "reviewed_sources_sha256": SOURCE_HASHES,
        "dispatch_and_handler_instructions": checked,
        "handler_bytes_sha256": hashlib.sha256(pe.get_data(0x1D2B0, 0x29B)).hexdigest(),
        "effective_branch_ids": [
            "1006:literal-enemy-index", "1006:single-global-or-negative-self-index",
            "1006:double-global-or-negative-self-index", "1006:outside-battle-no-effect",
            "1006:party-escape-option-and-condition", "1006:troop-escape-option-and-condition",
            "1006:single-enemy-condition-active-positive-hp", "1006:single-enemy-invalid-index-error",
            "1006:unknown-target-no-effect",
        ],
        "parameter_rule": "A0 is literal target 0/1/2; other values perform no target effect. A1 is an index resolved before battle/target checks. A3 low nibble 0 is literal, 1 one numeric read, 2..15 two numeric reads. Negative references use self, with a negative first reference preserving nonpositive address sign for the second lookup. Missing A0/A1/A2/A3 are zero; zero/out-of-range numeric references resolve zero. Only A2 bit 0 is used; other option bits, A3 high bits, extra Args and Text have no payload effect.",
        "context_rule": "Shared dispatch has no owner-mode restriction. Parameter evaluation precedes the battle-active byte check at 0x279130. If the byte is zero the handler returns without battle effects. Map/common/battle and 3019-produced records therefore require contextual verification; base Player currently only dispatches this opcode in its battle interpreter.",
        "target_rules": {
            "party": "Target 0 skips only when A2 bit 0 is set and battle condition 0x2790ac is 4. Otherwise writes 100 to 0x279080 and zero to 0x279144; enumerates party actors and invokes 0x18eb90 only when predicate 0x924a0 is false.",
            "troop": "Target 1 skips only when A2 bit 0 is set and condition is 3. Otherwise writes 100 to 0x279080 and invokes 0x18eb90 for every enemy with +0xe8 nonzero and +0x20 positive.",
            "single_enemy": "Target 2 skips unconditionally in condition 3, regardless of A2. Otherwise interprets resolved index as unsigned for bounds checking; invalid/negative index enters nonreturning 0xc8fc0 error. Valid active (+0xe8) positive-HP (+0x20) target invokes 0x18eb90; this branch does not write the two party/troop control globals.",
        },
        "remaining_questions": [
            "W07-004: dynamically verify literal/single/double index selection, all A3 low nibbles, signed self references, short arrays, ignored data, and no-battle/unknown-target continuation in shared/static/3019 contexts.",
            "W07-005: dynamically confirm party/troop/single side effects, battle condition IDs 3/4 and option polarity, helper 0x924a0 predicate and 0x18eb90 timing/audio/status/escape outcome, dead/hidden actors and enemies, empty groups, invalid index error ordering, parent continuation and save/resume behavior.",
            "241028, Web and Android retain their independent version/platform scope; this audit does not establish parity for them.",
        ],
        "acceptance_ids": ["W07-004", "W07-005"],
        "player_gaps": [
            "Current battle handler reads literal A1 and ignores A3.",
            "Current option==0 check differs from original A2 bit-0 polarity for target 0/1; current target 2 option bypass differs from original unconditional condition-3 guard.",
            "Current invalid enemy index warns and continues; original selected invalid-index path errors.",
            "Party force_flee_enabled, enemy hidden/death timer and escape sound require outcome/timing verification against original helpers.",
        ],
        "scope": "250921 Windows opcode 1006 scope audit only. Parameters, top-level effects, helper consumers and explicit open proof questions are assigned; no Player feature is closed by this static evidence.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: nine branch groups; selector/context and battle outcome acceptance destinations")


if __name__ == "__main__":
    main()
