"""Audit the fixed 250921 opcode 40 runtime dispatch scope."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
CONTRACT = REFERENCE / "commands/00040.md"
DISPATCH = REFERENCE / "evidence/DispatchCommands-250921.md"
REPORT = REFERENCE / "analysis-work/reports/00040-8fc63ed1d0f184f84681df371a51c2a23492efcc87285134b3418b69b51dff68.json"
OUTPUT = ROOT / "docs/evidence/maniac-250921-00040-scope-audit.json"
EXPECTED_SHA256 = {
    RUNTIME: "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f",
    CONTRACT: "6426b9dbe0bce098bef8b1c3e0b281fab618b658738ac058eadf6edbdf1c8608",
    DISPATCH: "439c352c26011574534d0bd9da074b08ff33d37848e7a52461640102d9af1f7c",
}
SITES = {
    0x463E2: "mov eax, dword ptr [r13]",
    0x463E6: "sub eax, 1",
    0x463E9: "js 0x140047d15",
    0x463EF: "jmp 0x1400458e0",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    for path, expected in EXPECTED_SHA256.items():
        if sha256(path) != expected:
            raise ValueError(f"fixed 250921 reference changed: {path}")
    report = json.loads(REPORT.read_text(encoding="utf-8"))
    if not any(row.get("dispatch", {}).get("opcode") == 40
               and row["dispatch"].get("branch_rva") == "0x463e2"
               for row in report["inputs"]["deferred"]):
        raise ValueError("reviewed opcode 40 dispatch mapping changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    instructions = {}
    for rva, expected in SITES.items():
        decoded = list(decoder.disasm(image.get_data(rva, 16), image.OPTIONAL_HEADER.ImageBase + rva, count=1))
        if len(decoded) != 1:
            raise ValueError(f"missing opcode 40 dispatch instruction {rva:#x}")
        actual = f"{decoded[0].mnemonic} {decoded[0].op_str}"
        if actual != expected:
            raise ValueError(f"opcode 40 dispatch changed at {rva:#x}: {actual}")
        instructions[f"0x{rva:x}"] = {"instruction": actual, "bytes": decoded[0].bytes.hex()}
    if sum(len(bytes.fromhex(row["bytes"])) for row in instructions.values()) != 0x12:
        raise ValueError("opcode 40 dispatch window has gaps or overlap")
    audit = {
        "runtime_sha256": EXPECTED_SHA256[RUNTIME],
        "reviewed_contract_sha256": EXPECTED_SHA256[CONTRACT],
        "reviewed_dispatch_sha256": EXPECTED_SHA256[DISPATCH],
        "reviewed_report_sha256": sha256(REPORT),
        "dispatch_instructions": instructions,
        "effective_branch_ids": ["40:valid-frame-next-fetch"],
        "invalid_state_branch": "Zero interpreter-frame depth jumps to 0x47d15, an invalid execution state rather than an event-command mode.",
        "parameter_rule": "The opcode 40 runtime block reads frame depth only. It ignores the encoded record's Args and Text, whose bytes and indent must remain intact for serialization and parent scans. The shared dispatcher has advanced the cursor before this block and refetches in the same invocation.",
        "formatter_boundary": "The reviewed default_formatter at 0x16cb0 displays generic Text and Args for editing; it is not the runtime consumer at 0x463e2.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "runtime_context_evidence": "The shared 250921 opcode-specific dispatch block has no interpreter-mode guard. Dynamic original/Player parity in each context remains W02-022.",
        "remaining_questions": [
            "Original and Player same-invocation next-command scheduling, parent scan and retained Args/Text/indent parity across map/common/battle remain W02-022.",
            "241028 consumers and Web/Android execution remain separate version/platform acceptance scopes.",
        ],
        "acceptance_ids": ["W02-022"],
        "scope": "Fixed 250921 Windows opcode 40 effective runtime branch scope. This audit does not claim Player feature parity.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: one valid-frame next-fetch branch; invalid depth separated")


if __name__ == "__main__":
    main()
