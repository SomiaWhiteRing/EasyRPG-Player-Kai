"""Pin the 250921 End Load Process flag and its direct consumers."""

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
SOURCE = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-3004-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
REPORT = "analysis-work/reports/03004-f1b9bb43b38e9403a005a098174083bc62dd119f393f919d7c904fccda32f0b0.json"
SOURCES = {
    "commands/03004.md": "70e2819bc52e956a238ddb35dfb9a8a8a0ccc9671206ace88c60547e5b65f18b",
    "evidence/DispatchCommands-250921.md": "439c352c26011574534d0bd9da074b08ff33d37848e7a52461640102d9af1f7c",
    REPORT: "770e85453a69ccd1365eeac570a5c06831f9fbe2f4d9efc90556deaec96d6fa3",
}
BRANCH = "3004:set-end-load-flag-and-success"
FEATURES = ("W11-027", "W11-028")
SITES = {
    0x4591D: "mov edx, dword ptr [rbx]",
    0x45921: "and edx, 0x3ff",
    0x45927: "shr rax, 8",
    0x4592E: "shl ecx, 3",
    0x45931: "sub edx, ecx",
    0x45933: "add edx, 0x19a",
    0x459A7: "movzx eax, byte ptr [r9 + rax + 0x47df8]",
    0x459B0: "mov ecx, dword ptr [r9 + rax*4 + 0x47d54]",
    0x459BB: "jmp rcx",
    0x46842: "mov byte ptr [rip + 0x232921], 1",
    0x46849: "mov al, 1",
    0x4684B: "jmp 0x140046ef3",
    0x46F0A: "ret",
    0x2DD1C: "cmp byte ptr [rip + 0x24b448], bl",
    0x2DD22: "sete dil",
    0x192BA9: "cmp byte ptr [rip + 0xe65b8], 0",
    0x192BB0: "mov byte ptr [rip + 0xe65b2], 0",
    0x192BB7: "jne 0x140192bf7",
    0x192BB9: "cmp byte ptr [rip + 0xe65aa], 0",
    0x192BC0: "jne 0x140192bd5",
    0x192BC2: "movzx eax, byte ptr [rip + 0xe65a2]",
    0x192BC9: "inc al",
    0x192BCB: "mov byte ptr [rip + 0xe659a], al",
    0x192BD1: "cmp al, 1",
    0x192BD3: "jbe 0x140192bf7",
    0x192BE4: "mov byte ptr [rip + 0xe657f], 1",
    0x192BEB: "call 0x1401849d0",
    0x192BF0: "mov byte ptr [rip + 0xe6571], 1",
    0x184A0A: "mov eax, dword ptr [rdi + 0x20]",
    0x184A0D: "cmp eax, 0x64",
    0x599FB: "mov dword ptr [rip + 0x21f763], 0",
    0x5AA88: "xor edi, edi",
    0x5AB43: "mov dword ptr [rip + 0x21e61f], edi",
    0x19299E: "mov dword ptr [rip + 0xe67c0], 0",
    0x192A51: "mov dword ptr [rip + 0xe670d], 0",
}


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
            raise ValueError(f"reviewed 3004 source changed: {name}")
    report = json.loads((SOURCE / REPORT).read_text(encoding="utf-8"))
    if not any(entry.get("dispatch", {}).get("opcode") == 3004
               and entry["dispatch"].get("branch_rva") == "0x46842"
               for entry in report["inputs"]["deferred"]):
        raise ValueError("reviewed opcode mapping changed")
    if [(entry["rva"], entry["instruction"]) for entry in report["inputs"]["disassembly"][0]["instructions"]] != [
            ("0x46842", SITES[0x46842]), ("0x46849", SITES[0x46849]),
            ("0x4684b", SITES[0x4684B])]:
        raise ValueError("reviewed full success block changed")

    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    base = image.OPTIONAL_HEADER.ImageBase
    observed = {}
    for rva, expected in SITES.items():
        insns = list(decoder.disasm(image.get_data(rva, 16), base + rva, count=1))
        if len(insns) != 1:
            raise ValueError(f"no instruction at {rva:#x}")
        insn = insns[0]
        actual = f"{insn.mnemonic} {insn.op_str}".strip()
        if actual != expected:
            raise ValueError(f"3004 chain changed at {rva:#x}: {actual}")
        observed[f"0x{rva:x}"] = {"instruction": actual, "bytes": insn.bytes.hex()}

    normalized = (3004 & 0x3FF) - (((3004 >> 8) & 0xFF) << 3) + 0x19A
    if not 0x444 < normalized < 0x56F:
        raise ValueError("3004 left selected dispatcher table")
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + slot * 4
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x4FE, 0x47EA8, 10, 0x47D7C, 0x46842):
        raise ValueError("3004 table route changed")

    # An aligned .pdata sweep catches direct RIP references and overlapping
    # four-byte clears. It cannot prove the absence of indirect alias writes.
    references = []
    for entry in image.DIRECTORY_ENTRY_EXCEPTION:
        lo, hi = entry.struct.BeginAddress, entry.struct.EndAddress
        for insn in decoder.disasm(image.get_data(lo, hi - lo), base + lo):
            for operand in insn.operands:
                if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
                    continue
                address = insn.address + insn.size + operand.mem.disp - base
                if 0x279168 <= address <= 0x27916B:
                    references.append({"rva": f"0x{insn.address-base:x}",
                                       "target_rva": f"0x{address:x}",
                                       "instruction": f"{insn.mnemonic} {insn.op_str}"})
    reference_rvas = {int(ref["rva"], 16) for ref in references}
    required = {0x2DD1C, 0x46842, 0x192BA9, 0x192BB9, 0x192BC2,
                0x192BCB, 0x192BE4, 0x192BF0, 0x599FB, 0x5AB43,
                0x19299E, 0x192A51}
    if not required <= reference_rvas:
        raise ValueError("flag consumer/reset reference missing")

    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inv = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
               if row["opcode"] == "3004")
    candidate = next(row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                     if row["ID"] == "3004:command")
    if (items["C250921-03004"]["status"] != "closed"
            or any(items[key]["parent_id"] != "W11-006" or items[key]["status"] != "open"
                   for key in FEATURES)
            or inv["effective_branch_ids"] != BRANCH
            or inv["acceptance_ids"] != ";".join(FEATURES)
            or candidate["acceptance_ids"] != ";".join(FEATURES)):
        raise ValueError("3004 scope and acceptance mapping incomplete")

    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "opcode_mapping": {"opcode": 3004, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}",
                           "branch_rva": f"0x{target:x}"},
        "fixed_instructions": observed,
        "rip_references_to_279168_through_27916b": references,
        "effective_branch_ids": [BRANCH],
        "acceptance_ids": list(FEATURES),
        "command_rule": "Shared dispatcher selects RVA 0x46842. The full block writes byte 27916a=1, returns AL=1 through the common epilogue, and neither reads Args/Text nor waits. Zero, short, extended or malformed Args and arbitrary Text cannot alter this opcode-specific path; retain original record, indent and payload for serialization and parent scans.",
        "conditional_consumer": "Conditional Branch type 12, operand 0 compares 27916a with zero at 0x2dd1c and sets its Boolean result at 0x2dd22. Thus before End Load Process or map first-update completion it can report right-after-load; after either flag setter it reports false, subject to the surrounding conditional command's own argument rules.",
        "map_consumer": "At 0x192ba9, map update gates on byte 279168==0. It resets 279169, then 27916a!=0 bypasses the small 27916b delay. With flag zero it increments 27916b and skips the transition while the new count <=1. On transition it sets 27916a=1, calls brightness/presentation helper 0x1849d0 with 279150, and sets 279168=1. Later updates bypass this startup block until a reset clears 279168.",
        "reset_boundary": "Four-byte zero stores at startup 0x599fb and map setup/teardown 0x19299e/0x192a51 clear 279168..27916b together; restart path 0x5ab43 writes zero-valued EDI to the same four-byte group. Their exact event/frame timing and all indirect aliases still require runtime confirmation under W11-028.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "context_evidence": "The opcode-specific block has no context guard. These are shared-dispatch candidate contexts; no original/Player execution comparison is claimed by this audit.",
        "remaining_questions": [
            "W11-027: original/Player conditional type-12 operand-0 values before/after 3004, repeated 3004, no-execution path, Args/Text preservation and frame continuation in applicable contexts.",
            "W11-028: original/Player next-map-update delay/brightness/screen trajectory, reset timing, save/restart persistence and interaction with 3003 A3, including existing W11-015/016/025/026 integration boundaries.",
            "241028 consumer behavior and Web/Android acceptance remain separate.",
        ],
        "scope": "250921 Windows static branch/consumer scope only; Player functionality and dynamic parity remain open.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: opcode 3004 flag and direct consumers pinned; two feature leaves open")


if __name__ == "__main__":
    main()
