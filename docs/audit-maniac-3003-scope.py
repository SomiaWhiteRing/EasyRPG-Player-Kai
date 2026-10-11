"""Bind the 250921 Load command branches to the original executable."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete/evidence/NativeBatch10-250921.md"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-3003-scope-audit.json"
RUNTIME_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCE_SHA256 = "f79ad5bea9a3770161d89919a9acb8219adbd6a6838de3cc0cc74f020562537f"
INSTRUCTIONS = {
    0x46787: "mov eax, dword ptr [rbx + 0x20]",  # original record count
    0x467A2: "je 0x47d01",                      # empty record returns success
    0x467AC: "cmp dword ptr [rax], r15d",       # A0 literal/indirect
    0x467B7: "call 0x17d7c0",                   # negative self source
    0x467E6: "test edi, edi",                   # resolved nonpositive slot
    0x467F1: "cmp edx, 2",                      # A2 missing defaults zero
    0x4680B: "test r15d, r15d",                 # A3 reset flag
    0x46812: "test ecx, ecx",                   # A2 bypass gate
    0x4682A: "call 0x82bc0",                   # existence/header precheck
    0x46835: "test al, al",                     # precheck result
    0x47D01: "mov al, 1",                      # normal skip success
    0x47D28: "movzx ecx, sil",                  # A3 passed to restart
    0x47D2C: "mov dword ptr [rip + 0x22188a], edi",  # target slot
    0x47D32: "call 0x53c10",                   # engine control restart
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    if sha256(RUNTIME) != RUNTIME_SHA256 or sha256(SOURCE) != SOURCE_SHA256:
        raise ValueError("250921 runtime or reviewed Load consumer changed")
    source_text = SOURCE.read_text(encoding="utf-8")
    if "## 3003 — direct load" not in source_text or "Native writer002fd8f4" not in source_text:
        raise ValueError("reviewed Load source section changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    observed = {}
    for rva, expected in INSTRUCTIONS.items():
        decoded = list(decoder.disasm(image.get_data(rva, 16), rva, count=1))
        if len(decoded) != 1:
            raise ValueError(f"missing Load instruction at {rva:#x}")
        actual = f"{decoded[0].mnemonic} {decoded[0].op_str}"
        if actual != expected:
            raise ValueError(f"Load branch changed at {rva:#x}: {actual}")
        observed[f"0x{rva:x}"] = actual
    audit = {
        "original_runtime_sha256": RUNTIME_SHA256,
        "reviewed_source_sha256": SOURCE_SHA256,
        "dispatch_instructions": observed,
        "effective_branch_ids": [
            "3003:literal-or-one-lookup-global-self-slot",
            "3003:empty-short-nonpositive-no-load",
            "3003:header-check-or-bypass",
            "3003:restart-with-reset-presentation-flag",
            "3003:contextual-frame-and-state-restoration",
        ],
        "parameter_rule": "A0=0 takes A1 literally; every nonzero A0 reads A1 as a numeric variable address once, including negative self. Empty record returns; other missing fields read zero. A2=0 requests the header/existence precheck, any nonzero A2 bypasses it. A3 controls reset presentation/timing and defaults zero when absent. Text and extra Args have no consumer in this branch.",
        "lifecycle_rule": "A nonpositive resolved slot or failed precheck returns immediate success with no load. Otherwise the dispatcher stores the slot and invokes the engine control restart at RVA 0x53c10; it does not return through ordinary event-command success. The catch/restart path reinitializes game state and loads the target slot. This creates context-dependent continuation and save compatibility requirements.",
        "remaining_questions": [
            "Fixed original/Player literal and one-lookup global/self slot tests, short/invalid slots and Text preservation",
            "Checked and bypassed A2 behavior for present, missing and malformed files without equating header existence with valid payload",
            "A3=0/1 presentation timing, reset brightness and control-exception behavior",
            "Map foreground, parallel, common, battle and 3019-generated candidate load contexts, plus restored frame and state fidelity",
            "Cross-runtime load/save interchange, 241028 behavior, Web and Android limits separately scoped",
        ],
        "acceptance_ids": ["W11-011", "W11-012", "W11-013", "W11-014", "W11-015", "W11-016",
                           "W11-017", "W11-023", "W11-024", "W11-025", "W11-026"],
        "scope": "250921 Windows Load branch and restart-path inventory only. Prior 3003 fixed self-bank continuations close narrow interchange leaves; they do not prove all slot modes, header bypass, A3 timing or event contexts.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(audit['effective_branch_ids'])} original Load branch groups")


if __name__ == "__main__":
    main()
