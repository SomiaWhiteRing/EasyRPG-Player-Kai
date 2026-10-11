"""Pin the 250921 Get Mouse Position runtime branches and direct helpers."""

from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-3005-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCES = {
    "commands/03005.md": "15e461d2c02a9d8469104f5f1031434acbfc28867246248eec7d36e88c8e0c84",
    "evidence/runtime-250921/00024bf0.c": "6e5ed29f9e26e484d2383175094ae89a2f8d9a282531a5f02fc638da4b68cff1",
    "evidence/native-250921/00034410.c": "c670fb474a965a8dbe2f4f926aa9062a5b7ff4d1ded3d1d8d873b41a3ed5f8b9",
    "evidence/DispatchCommands-250921.md": "439c352c26011574534d0bd9da074b08ff33d37848e7a52461640102d9af1f7c",
}
WINDOWS = (
    (0x24BF0, 0x24D1E, "97999475f605e16085f3c2311dd24a54c8498d23e519f98000470b0ca7d8ea49"),
    (0x8C9E0, 0x8CABC, "b1c187ba8758cf9000654e45865944eccd762874a7d064e937d7f70a08f66b2e"),
    (0x5ACA7, 0x5AD17, "0aacff11303f947122198f33e1d05a4b9a41be75e0b33631830a6cfad4b1a3d0"),
    (0x46197, 0x461A4, "cba01c8cc78381577baa5ff6e22b15a58388a96279a59e1a495432f0423875f7"),
    (0x463CE, 0x463F4, "3da0d6b60c895d16706be68bfa066eb223733d36324988829c76b03ba75d3c58"),
)
SITES = {
    0x24C0E: "cmp dword ptr [r8 + 0x20], 3",
    0x24C1B: "jbe 0x140024c43",
    0x24C21: "mov ebx, dword ptr [rax + 0xc]",
    0x24C24: "call qword ptr [rip + 0x1daad6]",
    0x24C36: "call qword ptr [rip + 0x1daaa4]",
    0x24C3C: "test bl, 1",
    0x24C3F: "jne 0x140024c99",
    0x24C43: "call qword ptr [rip + 0x1daab7]",
    0x24C55: "call qword ptr [rip + 0x1daa85]",
    0x24C65: "mulsd xmm0, qword ptr [rip + 0x244953]",
    0x24C6D: "call 0x1401d87e0",
    0x24C84: "mulsd xmm0, qword ptr [rip + 0x24493c]",
    0x24C8C: "call 0x1401d87e0",
    0x24C9B: "cmp dword ptr [rsi + 0x20], ebx",
    0x24CAE: "call 0x14008c9e0",
    0x24CB3: "cmp dword ptr [rsi + 0x20], 1",
    0x24CC8: "call 0x14008c9e0",
    0x24CCD: "cmp byte ptr [rip + 0x244777], bl",
    0x24CD3: "jne 0x140024d01",
    0x24CF4: "call 0x140061b20",
    0x24D1D: "ret",
    0x8C9EF: "test ecx, ecx",
    0x8C9F1: "jns 0x14008ca77",
    0x8CA69: "mov dword ptr [rax + rcx*4], ebp",
    0x8CA77: "je 0x14008cab1",
    0x8CAAE: "mov dword ptr [rdx + rdi*4], ebp",
    0x5ACA7: "mov eax, dword ptr [rip + 0x20e80b]",
    0x5ACB9: "movsd qword ptr [rip + 0x20e8ff], xmm2",
    0x5ACD7: "divsd xmm1, xmm0",
    0x5ACDB: "movsd qword ptr [rip + 0x20e8dd], xmm1",
    0x5ACED: "movsd qword ptr [rip + 0x20e8d3], xmm2",
    0x5AD0B: "divsd xmm1, xmm0",
    0x5AD0F: "movsd qword ptr [rip + 0x20e8b1], xmm1",
    0x46197: "mov r8, rbx",
    0x4619A: "call 0x140024bf0",
    0x4619F: "jmp 0x1400463ce",
    0x463CE: "cmp byte ptr [rip + 0x223075], r15b",
    0x463D5: "je 0x140047d01",
    0x463EF: "jmp 0x1400458e0",
}
BRANCHES = (
    "3005:scaled-client-position",
    "3005:raw-client-position",
    "3005:signed-X-then-Y-destinations",
    "3005:pf-zero-event-refresh-success",
    "3005:pf-nonzero-no-refresh-refetch",
)
FEATURES = ("W10-004", "W10-005")
BRANCH_ACCEPTANCE = {branch: "W10-004" if index < 3 else "W10-005"
                     for index, branch in enumerate(BRANCHES)}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(path: Path, delimiter: str = ",") -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def main() -> None:
    if sha(RUNTIME) != RUNTIME_SHA:
        raise ValueError("fixed original runtime changed")
    for name, expected in SOURCES.items():
        if sha(SOURCE / name) != expected:
            raise ValueError(f"reviewed 3005 source changed: {name}")
    image = pefile.PE(str(RUNTIME))
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    observed = {}
    for lo, hi, expected_sha in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if hashlib.sha256(blob).hexdigest() != expected_sha:
            raise ValueError(f"3005 machine window changed at {lo:#x}")
        instructions = list(decoder.disasm(blob, base + lo))
        if sum(insn.size for insn in instructions) != hi - lo:
            raise ValueError(f"3005 machine window incomplete at {lo:#x}")
        records = []
        for insn in instructions:
            rva = insn.address - base
            actual = f"{insn.mnemonic} {insn.op_str}".strip()
            if rva in SITES:
                if actual != SITES[rva]:
                    raise ValueError(f"3005 instruction changed at {rva:#x}: {actual}")
                observed[rva] = actual
            records.append({"rva": f"0x{rva:x}", "instruction": actual,
                            "bytes": insn.bytes.hex()})
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected_sha, "instructions": records})
    if set(observed) != set(SITES):
        raise ValueError("3005 key instruction outside frozen windows")

    normalized = (3005 & 0x3FF) - (((3005 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x4FF, 0x47EA9, 11, 0x47D80, 0x46197):
        raise ValueError("3005 opcode-to-consumer route changed")
    imports = {imp.address - base: (module.dll.decode(), imp.name.decode())
               for module in image.DIRECTORY_ENTRY_IMPORT for imp in module.imports if imp.name}
    if imports.get(0x1FF700) != ("USER32.dll", "GetCursorPos") or imports.get(0x1FF6E0) != (
            "USER32.dll", "ScreenToClient"):
        raise ValueError("3005 OS coordinate imports changed")
    if (struct.unpack("<II", image.get_data(0x257DF8, 8)) != (240, 320)
            or struct.unpack("<d", image.get_data(0x2322B0, 8))[0] != 1.0):
        raise ValueError("3005 scaling constants changed")

    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3005")
    candidate = next(row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                     if row["ID"] == "3005:command")
    if (items["C250921-03005"]["status"] != "closed"
            or items["W10-001"]["item_kind"] != "summary"
            or items["W10-006"]["item_kind"] != "unexpanded"
            or any(items[key]["parent_id"] != "W10-001" or items[key]["status"] != "open"
                   for key in FEATURES)
            or inventory["effective_branch_ids"] != ";".join(BRANCHES)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or candidate["acceptance_ids"] != ";".join(FEATURES)):
        raise ValueError("3005 acceptance mapping incomplete")

    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3005, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}"},
        "imports": {"GetCursorPos": "USER32.dll IAT RVA 0x1ff700",
                    "ScreenToClient": "USER32.dll IAT RVA 0x1ff6e0"},
        "effective_branch_ids": list(BRANCHES),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(FEATURES),
        "coordinate_rule": "Always calls GetCursorPos, then ScreenToClient on window handle 269638. With fewer than four Args or A3 bit 0 clear, multiplies client X and Y by factors 2695c0/2695c8 and rounds through 1d87e0 before signed-int conversion. With at least four Args and A3 bit 0 set, keeps unscaled client coordinates. Other A3 bits, A2, further Args and Text have no opcode-specific read.",
        "factor_rule": "Restart/display setup at 5aca7..5ad0f writes 2695c0 = 320 / positive client width 2694b8 and 2695c8 = 240 / positive client height 2694bc; nonpositive dimensions use double 1.0. The fixed 320/240 and 1.0 bytes are checked. Exact DPI, black bars, focus and OS failure behavior remain dynamic acceptance.",
        "destination_rule": "A0 and A1 are taken only when present, otherwise zero. The command calls 8c9e0 first for X, then Y. The helper treats negative signed addresses as current-self bank, zero as no write, positive as global numeric bank; it grows the relevant vector before assignment. INT_MIN and aliasing are explicit boundary acceptance, not inferred from the normal path.",
        "refresh_and_schedule_rule": "When configuration byte 26944a is zero, the consumer calls 61b20 once per current-map event after both writes; nonzero skips this loop. Shared post-call gate at 463ce returns success on zero and refetches next command within the same invocation on nonzero, subject to frame-depth guard. 26944a is the pf configuration byte established by the separate 3022 scope audit.",
        "record_rule": "There is no four-argument minimum: count 0 defaults both destinations to zero; count 1 defaults Y to zero; count 2/3 has both destinations and scaled coordinates; count >=4 interprets only A3 bit 0. Preserve all original Args, Text and indent. Formatter/managed-editor PosType is not a runtime branch.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "context_evidence": "No 3005-specific context guard occurs between shared selector and consumer. These are static shared-dispatch candidates; this audit contains no original/Player execution comparison.",
        "remaining_questions": [
            "W10-004: fixed original/Player scaled/raw client X/Y under window size, DPI, black bars and focus states; signed/zero destinations, short/extra payloads, Text/indent retention and X-before-Y alias effects.",
            "W10-005: both pf configurations with map event refresh and same-invocation scheduling in applicable contexts; save/restart continuation and Player unsupported-platform policy.",
            "W10-006 retains other 250921 Windows input/console commands and shared controls; 241028, Web and Android remain separate.",
        ],
        "scope": "250921 Windows static Get Mouse Position command scope only; Player implementation and dynamic parity remain open.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: five 3005 branch groups mapped to two open feature leaves")


if __name__ == "__main__":
    main()
