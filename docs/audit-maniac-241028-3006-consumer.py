"""Bind 241028 Set Mouse Position's consumer and old/new differential."""

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
NEW_AUDIT = ROOT / "docs/evidence/maniac-250921-3006-scope-audit.json"
OUTPUT = ROOT / "docs/evidence/maniac-241028-3006-consumer-audit.json"
OLD_SHA = "49ba424f3ff1fa7d4fc1334eea272fde4e437d07a6901dc6b87efb95383ec44d"
NEW_AUDIT_SHA = "95e4674a3ea7be3667beba8b24d089c0f5542e09d7e39e25d41bd51c9a63d886"
WINDOWS = (
    (0x22A20, 0x22B5E, "02c99c3e1d121e42396c6508dc4aeb020f2143887e36b50ebf546e77be8a1b6e"),
    (0x3E9C7, 0x3E9D4, "4bfa80781fb1900639e50b74a9c2556ae6f5a889e116dd62897cb279a5c59ee2"),
    (0x3E836, 0x3E849, "2c5a85bad4ca0884e74279fd573319bd64d9104a72bfec174e4f864d50149e12"),
    (0x50017, 0x50087, "99f3d6117765d5f48888f34225502031ccf0e4e4ba6d08d13bf5d0c42686a158"),
)
SITES = {
    0x22A33: "mov r9d, dword ptr [r8 + 0x18]",
    0x22A40: "cmp dword ptr [rax], 0",
    0x22A43: "je 0x140022aa0",
    0x22A59: "movsxd rcx, dword ptr [rax + 4]",
    0x22A5D: "test ecx, ecx",
    0x22A6B: "cmp eax, ecx",
    0x22A6D: "jbe 0x140022a75",
    0x22A6F: "mov ecx, dword ptr [r10 + rcx*4]",
    0x22A85: "movsxd r8, dword ptr [rax + 8]",
    0x22A91: "sar rdx, 2",
    0x22A95: "cmp edx, r8d",
    0x22A98: "jbe 0x140022abe",
    0x22A9A: "mov edx, dword ptr [r10 + r8*4]",
    0x22AA6: "mov ecx, dword ptr [rax + 4]",
    0x22AB3: "mov edx, dword ptr [rax + 8]",
    0x22AC0: "mov eax, dword ptr [rip + 0x2233b6]",
    0x22ACC: "test ecx, ecx",
    0x22AD0: "cmp ecx, eax",
    0x22AE1: "divsd xmm0, qword ptr [rip + 0x234a87]",
    0x22AE9: "call 0x1401c4e68",
    0x22AFA: "mov eax, dword ptr [rip + 0x223378]",
    0x22B02: "test ecx, ecx",
    0x22B06: "cmp ecx, eax",
    0x22B17: "divsd xmm0, qword ptr [rip + 0x234a59]",
    0x22B1F: "call 0x1401c4e68",
    0x22B38: "call qword ptr [rip + 0x1cbbaa]",
    0x22B46: "call qword ptr [rip + 0x1cbba4]",
    0x22B5D: "ret",
    0x3E9C7: "mov r8, rbx",
    0x3E9CA: "call 0x140022a20",
    0x3E9CF: "jmp 0x14003e836",
    0x3E836: "cmp byte ptr [rip + 0x210bfe], r12b",
    0x3E83D: "je 0x14004067c",
    0x50017: "mov eax, dword ptr [rip + 0x20744b]",
    0x50033: "movd xmm1, dword ptr [rip + 0x1f5e41]",
    0x50047: "divsd xmm1, xmm0",
    0x5004B: "movsd qword ptr [rip + 0x20751d], xmm1",
    0x50053: "mov eax, dword ptr [rip + 0x207413]",
    0x50067: "movd xmm1, dword ptr [rip + 0x1f5e09]",
    0x5007B: "divsd xmm1, xmm0",
    0x5007F: "movsd qword ptr [rip + 0x2074f1], xmm1",
}
DIFFERENTIAL = "241028:3006-scaled-positive-global-source-only"
DESTINATION = "W12-012"


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(path: Path, delimiter: str = ",") -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def main() -> None:
    if sha(OLD) != OLD_SHA or sha(NEW_AUDIT) != NEW_AUDIT_SHA:
        raise ValueError("fixed 241028 runtime or reviewed 250921 audit changed")
    newer = json.loads(NEW_AUDIT.read_text(encoding="utf-8"))
    if newer["opcode_mapping"]["branch_rva"] != "0x461a4":
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
                    raise ValueError(f"241028 3006 instruction changed at {rva:#x}: {actual}")
                observed[rva] = actual
            records.append({"rva": f"0x{rva:x}", "instruction": actual,
                            "bytes": insn.bytes.hex()})
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected_sha, "instructions": records})
    if set(observed) != set(SITES):
        raise ValueError("key 241028 instruction outside frozen windows")
    normalized = (3006 & 0x3FF) - (((3006 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x40764 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x406CC + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x500, 0x40816, 12, 0x406FC, 0x3E9C7):
        raise ValueError("241028 opcode-to-consumer route changed")
    imports = {imp.address - base: (module.dll.decode(), imp.name.decode())
               for module in image.DIRECTORY_ENTRY_IMPORT for imp in module.imports if imp.name}
    if (imports.get(0x1EE6E8) != ("USER32.dll", "ClientToScreen")
            or imports.get(0x1EE6F0) != ("USER32.dll", "SetCursorPos")):
        raise ValueError("241028 cursor imports changed")
    if (struct.unpack("<i", image.get_data(0x245E7C, 4))[0] != 320
            or struct.unpack("<i", image.get_data(0x245E78, 4))[0] != 240):
        raise ValueError("241028 game bounds changed")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    triage = next(row for row in rows(ROOT / "docs/maniac-241028-consumer-triage.csv")
                  if row["opcode"] == "3006")
    if (items["C241028-03006"]["status"] != "closed"
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
        "opcode_mapping": {"opcode": 3006, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}",
                           "consumer_rva": "0x22a20"},
        "imports": {"ClientToScreen": "USER32.dll IAT RVA 0x1ee6e8",
                    "SetCursorPos": "USER32.dll IAT RVA 0x1ee6f0"},
        "differential_id": DIFFERENTIAL,
        "acceptance_ids": [DESTINATION],
        "old_rule": "Shared selector maps 3006 to 0x3e9c7, calls 0x22a20, then uses common pf gate 0x3e836. A0=0 uses literal A1/A2; every nonzero A0 uses one global-array lookup for A1/A2 when in unsigned bounds, so negative and out-of-range source addresses resolve to zero rather than current-self. Missing values resolve to zero. A3 and Text are not read. X then Y clamp to [0,319]/[0,239], divide by display factors 0x257570/0x257578, round via 0x1c4e68, call ClientToScreen then SetCursorPos; OS return values are ignored. Factor setup 0x50017..0x5007f uses 320/positive client width and 240/positive client height, else 1.0. pf zero returns success; nonzero enters shared next-fetch path. Original Args, Text and indent still require retention.",
        "new_rule_reference": "250921 audit shows signed current-self source helper, optional A3 bit0 raw/scaled path and shared pf scheduling; W10-007/008 remain open for Player and dynamic parity.",
        "effective_differences": ["241028 ignores A3 bit0 and always uses scaled game coordinates where 250921 permits raw client coordinates",
                                  "241028 nonzero-A0 variable mode reads only positive in-range global addresses; 250921 also resolves negative current-self addresses"],
        "shared_candidates": ["A0 zero literal versus nonzero variable mode", "missing A1/A2 becomes zero",
                              "X then Y resolution and clamp", "ClientToScreen then SetCursorPos",
                              "pf zero success versus nonzero shared refetch"],
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "context_evidence": "No old 3006-specific context guard exists between shared selector and consumer. Four contexts are static dispatcher candidates, not observed executions.",
        "remaining_questions": [
            "W12-012: fixed 241028/250921 originals and explicitly version-selected Player compare A0 global/current-self sources, A3 raw/scaled choice, coordinate/cursor effects, short/extra Args and Text, pf branches, applicable contexts and saved-cursor continuation.",
            "W10-007/008 retain 250921 Player function and scheduling; other 241028 commands, Web and Android remain independent.",
        ],
        "scope": "241028 Windows 3006 consumer decision only. No Player feature or dynamic original/Player acceptance is claimed.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: old 3006 consumer differs from 250921; W12-012 remains open")


if __name__ == "__main__":
    main()
