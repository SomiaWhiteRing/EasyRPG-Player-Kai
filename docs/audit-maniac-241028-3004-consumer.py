"""Bind 241028 End Load Process and its direct flag consumers."""

from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
import pefile


ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT.parent / "RPGRewriter-maniac/patch_maniacs_241028/English/RPG_RT.exe"
NEW_AUDIT = ROOT / "docs/evidence/maniac-250921-3004-scope-audit.json"
OUTPUT = ROOT / "docs/evidence/maniac-241028-3004-consumer-audit.json"
OLD_SHA = "49ba424f3ff1fa7d4fc1334eea272fde4e437d07a6901dc6b87efb95383ec44d"
NEW_AUDIT_SHA = "bdbf1eee6f09d3d482c2ec3a0b2ecc643d926941b1073c408e96e761125af38b"
WINDOWS = (
    (0x3E130, 0x3E1E1, "2a33df40159de004d9fc28746d6705af74975d56cca8d3149c85b954fdebea8e"),
    (0x3F1D3, 0x3F1E1, "579599c496eb4e157f0d6889d168ecf87493c254d9fa47bd356d57a80607bed3"),
    (0x293BD, 0x2943D, "f49dc46981c8955a66e2d21e5afb53c179a823269c6399ed560e3bef26b055c3"),
    (0x1840F0, 0x18414D, "97f8608956f278e6a04ffeec07fcc441f35fac3ee2d59412d3fbb7eb724eeccb"),
)
SITES = {
    0x3E144: "lea eax, [r8 + 1]",
    0x3E148: "mov dword ptr [r15 + 0xc], eax",
    0x3E150: "and edx, 0x3ff",
    0x3E160: "sub edx, ecx",
    0x3E162: "add edx, 0x19a",
    0x3E1CB: "movzx eax, byte ptr [r9 + rax + 0x40764]",
    0x3E1D4: "mov ecx, dword ptr [r9 + rax*4 + 0x406cc]",
    0x3E1DF: "jmp rcx",
    0x3F1D3: "mov byte ptr [rip + 0x227f40], 1",
    0x3F1DA: "mov al, 1",
    0x3F1DC: "jmp 0x14003f813",
    0x293BD: "cmp r9d, 1",
    0x293C1: "jbe 0x14002942d",
    0x293C7: "mov ecx, dword ptr [rax + 4]",
    0x293CA: "test ecx, ecx",
    0x293CC: "je 0x14002942d",
    0x2942D: "cmp byte ptr [rip + 0x23dce6], 0",
    0x29434: "sete dil",
    0x1840FF: "cmp byte ptr [rip + 0xe3012], 0",
    0x184106: "mov byte ptr [rip + 0xe300c], 0",
    0x18410F: "cmp byte ptr [rip + 0xe3004], 0",
    0x184118: "movzx eax, byte ptr [rip + 0xe2ffc]",
    0x18411F: "inc al",
    0x184121: "mov byte ptr [rip + 0xe2ff4], al",
    0x184127: "cmp al, 1",
    0x18413A: "mov byte ptr [rip + 0xe2fd9], 1",
    0x184141: "call 0x140172f30",
    0x184146: "mov byte ptr [rip + 0xe2fcb], 1",
    0x4ECE0: "mov dword ptr [rip + 0x21842e], 0",
    0x4FEB3: "mov dword ptr [rip + 0x21725f], edi",
    0x183E4E: "mov dword ptr [rip + 0xe32c0], 0",
    0x183F01: "mov dword ptr [rip + 0xe320d], 0",
    0x18441B: "mov byte ptr [rip + 0xe2cf6], 0",
}
FLAG_GROUP = (0x267118, 0x26711B)
REQUIRED_REFS = {
    0x27348, 0x2942D, 0x3F1D3, 0x3FCDE, 0x4ECE0, 0x4FEB3,
    0x17AA1F, 0x183E4E, 0x183F01, 0x1840FF, 0x184106, 0x18410F,
    0x184118, 0x184121, 0x18413A, 0x184146, 0x18441B, 0x185574,
    0x1855E7, 0x18562C, 0x185773, 0x194CD7,
}
DESTINATION = "W12-013"
DECISION = "241028:3004-shared-flag-chain-dynamic-parity-open"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(path: Path, delimiter: str = ",") -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def main() -> None:
    if sha(OLD) != OLD_SHA or sha(NEW_AUDIT) != NEW_AUDIT_SHA:
        raise ValueError("fixed 241028 runtime or reviewed 250921 audit changed")
    newer = json.loads(NEW_AUDIT.read_text(encoding="utf-8"))
    if newer["opcode_mapping"]["branch_rva"] != "0x46842":
        raise ValueError("new-version comparison boundary changed")
    image = pefile.PE(str(OLD))
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    windows = []
    observed = {}
    for lo, hi, expected_sha in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if hashlib.sha256(blob).hexdigest() != expected_sha:
            raise ValueError(f"241028 machine window changed at {lo:#x}")
        instructions = list(decoder.disasm(blob, base + lo))
        if sum(insn.size for insn in instructions) != hi - lo:
            raise ValueError(f"241028 machine window incomplete at {lo:#x}")
        records = []
        for insn in instructions:
            rva = insn.address - base
            actual = f"{insn.mnemonic} {insn.op_str}".strip()
            if rva in SITES:
                if actual != SITES[rva]:
                    raise ValueError(f"241028 3004 instruction changed at {rva:#x}: {actual}")
                observed[rva] = actual
            records.append({"rva": f"0x{rva:x}", "instruction": actual,
                            "bytes": insn.bytes.hex()})
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected_sha, "instructions": records})
    for rva, expected in SITES.items():
        if rva in observed:
            continue
        insns = list(decoder.disasm(image.get_data(rva, 16), base + rva, count=1))
        if len(insns) != 1:
            raise ValueError(f"no 241028 instruction at {rva:#x}")
        actual = f"{insns[0].mnemonic} {insns[0].op_str}".strip()
        if actual != expected:
            raise ValueError(f"241028 3004 reference changed at {rva:#x}: {actual}")
        observed[rva] = actual
    normalized = (3004 & 0x3FF) - (((3004 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x40764 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x406CC + slot * 4
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x4FE, 0x40814, 10, 0x406F4, 0x3F1D3):
        raise ValueError("241028 opcode-to-flag route changed")
    references = []
    for entry in image.DIRECTORY_ENTRY_EXCEPTION:
        lo, hi = entry.struct.BeginAddress, entry.struct.EndAddress
        for insn in decoder.disasm(image.get_data(lo, hi - lo), base + lo):
            for operand in insn.operands:
                if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
                    continue
                address = insn.address + insn.size + operand.mem.disp - base
                if FLAG_GROUP[0] <= address <= FLAG_GROUP[1]:
                    references.append({"rva": f"0x{insn.address-base:x}",
                                       "target_rva": f"0x{address:x}",
                                       "instruction": f"{insn.mnemonic} {insn.op_str}"})
    if {int(ref["rva"], 16) for ref in references} != REQUIRED_REFS:
        raise ValueError("old flag direct-reference set changed")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    triage = next(row for row in rows(ROOT / "docs/maniac-241028-consumer-triage.csv")
                  if row["opcode"] == "3004")
    if (items["C241028-03004"]["status"] != "closed"
            or items[DESTINATION]["parent_id"] != "W12-001"
            or items[DESTINATION]["status"] != "open"
            or triage["decision"] != DECISION
            or triage["runtime_acceptance"] != DESTINATION
            or triage["semantic_status"] != "static_shared_rule_decided_dynamic_parity_open"):
        raise ValueError("old-version shared decision and acceptance destination incomplete")
    result = {
        "old_runtime_sha256": OLD_SHA,
        "reviewed_new_audit_sha256": NEW_AUDIT_SHA,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3004, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}"},
        "direct_references_to_267118_through_26711b": references,
        "decision": DECISION,
        "acceptance_ids": [DESTINATION],
        "old_command_rule": "The complete opcode-specific block 0x3f1d3..0x3f1e0 writes byte 0x26711a=1 and returns success through the shared epilogue. It reads no Args or Text and has no wait. The interpreter advances the cursor before selecting the block. Raw record fields and indent must remain serializable.",
        "old_conditional_consumer": "Old conditional branch path 0x293bd..0x2943c reads the flag at 0x2942d when the second argument is missing or zero and sets its Boolean result to flag==0. This corresponds to the 250921 type-12 operand-0 consumer; exact surrounding conditional rules remain dynamic acceptance.",
        "old_map_consumer": "Map first-update path 0x1840ff gates on 0x267118==0, resets 0x267119, bypasses the small 0x26711b delay when flag 0x26711a is nonzero, otherwise increments the delay and skips the transition for counts <=1. Transition sets 0x26711a=1, calls presentation helper 0x172f30 and sets 0x267118=1.",
        "old_reset_boundary": "Direct four-byte zero stores at 0x4ece0, 0x183e4e and 0x183f01 clear 0x267118..0x26711b; 0x4feb3 writes EDI over the group and 0x18441b clears the first-update byte. Exact invocation timing and indirect aliases need dynamic comparison.",
        "shared_static_rule": "The old flag setter, conditional zero test, map startup delay and direct reset pattern match the reviewed 250921 chain structurally, with relocated flag and helper addresses. No old-only semantic branch is established by this static evidence.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "context_evidence": "No opcode-specific context guard occurs in the old success block. Four contexts are static dispatcher candidates, not observed executions.",
        "remaining_questions": [
            "W12-013: fixed 241028 original and explicitly version-selected Player compare conditional before/after 3004, map first-update delay/presentation, reset and save/restart trajectory, Args/Text retention and applicable contexts against 250921 checkpoints.",
            "W11-027/028 retain 250921 Player and original dynamic acceptance; other 241028 consumers, Web and Android remain independent.",
        ],
        "scope": "241028 Windows 3004 static consumer decision only. Direct RIP sweep does not prove absence of indirect aliases; Player parity is not accepted.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: old 3004 shared flag chain decided; W12-013 dynamic parity open")


if __name__ == "__main__":
    main()
