"""Audit fixed 250921 End Nest (opcode 10) runtime scope."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
CONTRACT = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete/commands/00010.md"
END_MARKERS = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete/evidence/EndMarkers-250921.md"
REPORT = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete/analysis-work/reports/00010-799739e7d65768555a00507137b8d6b89d61b98a85e42f2a7a565facb83c84fc.json"
OUTPUT = ROOT / "docs/evidence/maniac-250921-00010-scope-audit.json"
RUNTIME_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
CONTRACT_SHA256 = "a6cd62fe77dbba2f9c556d5665bec6b531bf307fe9a36f59fd703bbb2979b9bb"
END_MARKERS_SHA256 = "0cb0a5130b1d4d13fbdecf8b7e61fd63e16842aac0462124be63872cd53cf207"
SITES = {
    0x463E2: "mov eax, dword ptr [r13]",
    0x463E6: "sub eax, 1",
    0x463E9: "js 0x140047d15",
    0x463EF: "jmp 0x1400458e0",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    if (sha256(RUNTIME) != RUNTIME_SHA256 or sha256(CONTRACT) != CONTRACT_SHA256
            or sha256(END_MARKERS) != END_MARKERS_SHA256):
        raise ValueError("fixed 250921 original or reviewed marker contracts changed")
    report = json.loads(REPORT.read_text(encoding="utf-8"))
    deferred = report["inputs"]["deferred"]
    if not any(row.get("dispatch", {}).get("opcode") == 10
               and row["dispatch"].get("branch_rva") == "0x463e2" for row in deferred):
        raise ValueError("reviewed opcode 10 dispatch site changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    instructions = {}
    for rva, expected in SITES.items():
        decoded = list(decoder.disasm(image.get_data(rva, 16), image.OPTIONAL_HEADER.ImageBase + rva, count=1))
        if len(decoded) != 1:
            raise ValueError(f"missing opcode 10 dispatch instruction {rva:#x}")
        actual = f"{decoded[0].mnemonic} {decoded[0].op_str}"
        if actual != expected:
            raise ValueError(f"opcode 10 dispatch changed at {rva:#x}: {actual}")
        instructions[f"0x{rva:x}"] = {"instruction": actual, "bytes": decoded[0].bytes.hex()}
    if sum(len(bytes.fromhex(row["bytes"])) for row in instructions.values()) != 0x12:
        raise ValueError("opcode 10 dispatch window has gaps or overlap")
    if "Code::END" not in (ROOT / "lib/liblcf/src/generated/lcf/rpg/eventcommand.h").read_text(encoding="utf-8"):
        raise ValueError("liblcf END enum absent")
    audit = {
        "runtime_sha256": RUNTIME_SHA256,
        "reviewed_contract_sha256": CONTRACT_SHA256,
        "reviewed_end_markers_sha256": END_MARKERS_SHA256,
        "reviewed_report_sha256": sha256(REPORT),
        "dispatch_instructions": instructions,
        "effective_branch_ids": ["10:valid-frame-next-fetch"],
        "invalid_state_branch": "A zero-depth interpreter reaches 0x47d15, an error path outside valid event execution; not a supported command mode.",
        "parameter_rule": "Opcode 10 is an encoded event-list record and may retain arbitrary legacy Args/Text/indent. This dispatcher block reads none of its Args or Text. The shared fetch has already advanced the cursor before dispatch; opcode 10 immediately returns to fetch in the same dispatcher invocation. The opcode 0 file terminator is a separate command.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "runtime_context_evidence": "The shared 250921 dispatch site is not mode-gated. Existing fixed map/common fixtures contain opcode 10 records, but this scope audit does not claim original/Player dynamic parity for all contexts.",
        "remaining_questions": [
            "Original and Player same-invocation next-command scheduling and parent scan behavior for opcode 10 across map/common/battle contexts, including unusual retained Args/Text/indent, require W02-021.",
            "Parent command-specific branch scans and loop/label behavior remain in W02-013 or other named command leaves; opcode 10 scope does not close them.",
            "241028 consumers and Web/Android execution remain separate version/platform acceptance scopes.",
        ],
        "acceptance_ids": ["W02-021"],
        "scope": "Fixed 250921 Windows opcode 10 End Nest effective runtime branch scope only. No Player feature parity is claimed.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: one valid-frame refetch branch; invalid-depth error separated")


if __name__ == "__main__":
    main()
