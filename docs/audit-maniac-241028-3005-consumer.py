"""Bind 241028 Get Mouse Position's inlined consumer and version delta."""

from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
OLD = ROOT.parent / "RPGRewriter-maniac/patch_maniacs_241028/English/RPG_RT.exe"
NEW_AUDIT = ROOT / "docs/evidence/maniac-250921-3005-scope-audit.json"
OUTPUT = ROOT / "docs/evidence/maniac-241028-3005-consumer-audit.json"
OLD_SHA = "49ba424f3ff1fa7d4fc1334eea272fde4e437d07a6901dc6b87efb95383ec44d"
NEW_AUDIT_SHA = "6fe51637879f78e80a6d533e80377d55410bb84e7789cca93e0236bfb4eeba46"
WINDOWS = (
    (0x3DE50, 0x3E130, "09ed3ca33f13d250f64fcfd7b95fadf42aaa786e29b49b787c61ffcb6f2518f9"),
    (0x3E130, 0x3E1E1, "2a33df40159de004d9fc28746d6705af74975d56cca8d3149c85b954fdebea8e"),
    (0x3E893, 0x3E9C7, "c9cf7136a07cb902568555c3e59e1f154512eabdba82fdafa8cd39b302e764de"),
    (0x3E836, 0x3E849, "2c5a85bad4ca0884e74279fd573319bd64d9104a72bfec174e4f864d50149e12"),
    (0x4067C, 0x40683, "af8bc720914ed8f844cff8ef9c78524b7bf6a6e48ea38a46eb3a6e8c7f43803c"),
    (0x50017, 0x50087, "99f3d6117765d5f48888f34225502031ccf0e4e4ba6d08d13bf5d0c42686a158"),
)
SITES = {
    0x3DEC6: "xor r12d, r12d",
    0x3E144: "lea eax, [r8 + 1]",
    0x3E148: "mov dword ptr [r15 + 0xc], eax",
    0x3E14C: "mov edx, dword ptr [rbx]",
    0x3E150: "and edx, 0x3ff",
    0x3E156: "shr rax, 8",
    0x3E15D: "shl ecx, 3",
    0x3E160: "sub edx, ecx",
    0x3E162: "add edx, 0x19a",
    0x3E1CB: "movzx eax, byte ptr [r9 + rax + 0x40764]",
    0x3E1D4: "mov ecx, dword ptr [r9 + rax*4 + 0x406cc]",
    0x3E1DF: "jmp rcx",
    0x3E897: "call qword ptr [rip + 0x1afe5b]",
    0x3E8A8: "call qword ptr [rip + 0x1afe2a]",
    0x3E8B7: "mulsd xmm0, qword ptr [rip + 0x218cb1]",
    0x3E8BF: "call 0x1401c4e68",
    0x3E8D4: "mulsd xmm0, qword ptr [rip + 0x218c9c]",
    0x3E8DC: "call 0x1401c4e68",
    0x3E8E8: "cmp dword ptr [rbx + 0x18], r12d",
    0x3E8F6: "movsxd rdi, dword ptr [rax]",
    0x3E8F9: "test edi, edi",
    0x3E932: "mov dword ptr [rdx + rdi*4], r14d",
    0x3E93D: "cmp dword ptr [rbx + 0x18], 1",
    0x3E947: "movsxd rbx, dword ptr [rax + 4]",
    0x3E94B: "test ebx, ebx",
    0x3E984: "mov dword ptr [rdx + rbx*4], esi",
    0x3E987: "cmp byte ptr [rip + 0x210aad], r12b",
    0x3E98E: "jne 0x14003e843",
    0x3E9B5: "call 0x140056b80",
    0x3E836: "cmp byte ptr [rip + 0x210bfe], r12b",
    0x3E83D: "je 0x14004067c",
    0x4067C: "mov al, 1",
    0x50017: "mov eax, dword ptr [rip + 0x20744b]",
    0x5001D: "movsd xmm2, qword ptr [rip + 0x1d0dcb]",
    0x50029: "movsd qword ptr [rip + 0x20753f], xmm2",
    0x50033: "movd xmm1, dword ptr [rip + 0x1f5e41]",
    0x5004B: "movsd qword ptr [rip + 0x20751d], xmm1",
    0x50053: "mov eax, dword ptr [rip + 0x207413]",
    0x5005D: "movsd qword ptr [rip + 0x207513], xmm2",
    0x50067: "movd xmm1, dword ptr [rip + 0x1f5e09]",
    0x5007F: "movsd qword ptr [rip + 0x2074f1], xmm1",
}
DIFFERENTIAL = "241028:3005-always-scaled-positive-global-only"
DESTINATION = "W12-011"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(path: Path, delimiter: str = ",") -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def main() -> None:
    if sha(OLD) != OLD_SHA or sha(NEW_AUDIT) != NEW_AUDIT_SHA:
        raise ValueError("fixed 241028 runtime or reviewed 250921 audit changed")
    newer = json.loads(NEW_AUDIT.read_text(encoding="utf-8"))
    if newer["opcode_mapping"]["branch_rva"] != "0x46197":
        raise ValueError("new-version comparison boundary changed")
    image = pefile.PE(str(OLD))
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
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
                    raise ValueError(f"241028 3005 instruction changed at {rva:#x}: {actual}")
                observed[rva] = actual
            records.append({"rva": f"0x{rva:x}", "instruction": actual,
                            "bytes": insn.bytes.hex()})
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected_sha, "instructions": records})
    if set(observed) != set(SITES):
        raise ValueError("key 241028 instruction outside frozen windows")
    normalized = (3005 & 0x3FF) - (((3005 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x40764 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x406CC + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x4FF, 0x40815, 11, 0x406F8, 0x3E893):
        raise ValueError("241028 opcode-to-inline-consumer route changed")
    imports = {imp.address - base: (module.dll.decode(), imp.name.decode())
               for module in image.DIRECTORY_ENTRY_IMPORT for imp in module.imports if imp.name}
    if (imports.get(0x1EE6F8) != ("USER32.dll", "GetCursorPos")
            or imports.get(0x1EE6D8) != ("USER32.dll", "ScreenToClient")):
        raise ValueError("241028 cursor imports changed")
    if (struct.unpack("<i", image.get_data(0x245E7C, 4))[0] != 320
            or struct.unpack("<i", image.get_data(0x245E78, 4))[0] != 240
            or struct.unpack("<d", image.get_data(0x220DF0, 8))[0] != 1.0):
        raise ValueError("241028 scale constants changed")

    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    triage = next(row for row in rows(ROOT / "docs/maniac-241028-consumer-triage.csv")
                  if row["opcode"] == "3005")
    if (items["C241028-03005"]["status"] != "closed"
            or items[DESTINATION]["parent_id"] != "W12-001"
            or items[DESTINATION]["status"] != "open"
            or triage["decision"] != DIFFERENTIAL
            or triage["runtime_acceptance"] != DESTINATION
            or triage["semantic_status"] != "static_consumer_decided_dynamic_parity_open"):
        raise ValueError("old-version decision and destination incomplete")
    result = {
        "old_runtime_sha256": OLD_SHA,
        "reviewed_new_audit_sha256": NEW_AUDIT_SHA,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3005, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}"},
        "imports": {"GetCursorPos": "USER32.dll IAT RVA 0x1ee6f8",
                    "ScreenToClient": "USER32.dll IAT RVA 0x1ee6d8"},
        "factor_rule": "Display setup 0x50017..0x5007f writes X factor 257570 = 320 / positive client width 257468 and Y factor 257578 = 240 / positive client height 25746c; nonpositive dimensions use double 1.0. The three fixed source constants are checked from the old image.",
        "differential_id": DIFFERENTIAL,
        "acceptance_ids": [DESTINATION],
        "old_rule": "Shared command fetch advances cursor, normalizes 3005 to selector 0x4ff, and jumps through slot 11 into inline block 0x3e893. It always calls GetCursorPos then ScreenToClient and multiplies both client coordinates by 257570/257578, with 1c4e68 rounding and signed-int conversion. No A3 option or A2/Text read occurs on this inline path. X then Y destinations are read only when count >0/>1; each signed destination <=0 is skipped, while positive destinations are grown as needed and written in the global numeric array. No current-self destination helper appears on this path. In pf=0 the block iterates current-map events through 56b80 then enters the shared success gate; in pf!=0 it skips refresh and enters the common next-fetch route. All original record fields must remain serializable.",
        "new_rule_reference": "250921 audit shows A3 bit0 raw-client branch, signed global/current-self destination helper 8c9e0 and two-branch pf scheduling. Scaling factors are likewise initialized from positive client dimensions or 1.0 fallback, with 320/240 constants; exact OS and display behavior still needs dynamic comparison.",
        "effective_differences": ["241028 always scales and ignores A3 bit0 where 250921 may use raw client coordinates",
                                  "241028 drops zero/negative X/Y destinations where 250921 signed helper can write current-self destinations"],
        "shared_candidates": ["GetCursorPos then ScreenToClient", "scaled branch under A3 bit0 clear",
                              "missing X/Y destination causes no write", "positive global X then Y writes",
                              "pf zero refresh and success versus nonzero no-refresh refetch"],
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "context_evidence": "No old 3005-specific context guard exists between shared opcode selection and inline block. Four contexts are static dispatcher candidates, not observed gameplay executions.",
        "remaining_questions": [
            "W12-011: fixed 241028/250921 original and version-selected Player coordinate, A3, zero/negative/positive destinations, short/extended Args/Text, X/Y order and pf branches, with actual event contexts and post-restart continuation.",
            "W10-004/005 retain 250921 Windows Player behavior; 241028 other commands, Web and Android remain independent.",
        ],
        "scope": "241028 Windows 3005 consumer decision only. No Player feature or dynamic original/Player acceptance is claimed.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: old 3005 consumer differs from 250921; W12-011 remains open")


if __name__ == "__main__":
    main()
