"""Bind 250921 Set Mouse Position operand, coordinate and exit branches."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3006-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCES = {
    "commands/03006.md": "a2a531e4ea8ab1a10532694a5ffb0d3196ad2bf8645d645200aedde78534917e",
    "evidence/runtime-250921/00024d20.c": "e355ee1c8ea6c97d02921e3a1b7e2683afac49e56f6f4efc260fa5e6d52d0bbc",
    "evidence/native-250921/00034590.c": "c4bb3795a492db90e4201f68c2620517a72b4ea950c16941b7c614aa7388a259",
    "evidence/DispatchCommands-250921.md": "439c352c26011574534d0bd9da074b08ff33d37848e7a52461640102d9af1f7c",
}
WINDOWS = (
    (0x24D20, 0x24F0C, "193aceef4817273116bc77365c4720aec35a7a308b316ee46f83039869b733e8"),
    (0x17D7C0, 0x17D839, "a804fe11db49d3cda38d3f91106c0ca5e50ef8af61291869fcf489741ed29473"),
    (0x461A4, 0x461B1, "3b47964c8fa052127bc0ce54ff5c1016e5994a4716dcec3165c1011b1ba206f6"),
    (0x463CE, 0x463F4, "3da0d6b60c895d16706be68bfa066eb223733d36324988829c76b03ba75d3c58"),
)
SITES = {
    0x24D39: "mov edx, dword ptr [r8 + 0x20]",
    0x24D40: "test edx, edx",
    0x24D42: "je 0x140024dfc",
    0x24D4C: "cmp dword ptr [rax], 0",
    0x24D4F: "je 0x140024de5",
    0x24D6C: "test ecx, ecx",
    0x24D72: "call 0x14017d7c0",
    0x24D95: "cmp eax, ecx",
    0x24D99: "mov ecx, dword ptr [r10 + rcx*4]",
    0x24DB5: "test r9d, r9d",
    0x24DC0: "call 0x14017d7c0",
    0x24DDD: "jbe 0x140024e02",
    0x24DDF: "mov r8d, dword ptr [r10 + r9*4]",
    0x24DED: "mov dword ptr [rsp + 0x20], ecx",
    0x24DF6: "mov r8d, dword ptr [rax + 8]",
    0x24DFC: "xor ecx, ecx",
    0x24E02: "xor r8d, r8d",
    0x24E0A: "cmp edx, 3",
    0x24E13: "test byte ptr [rax + 0xc], 1",
    0x24E17: "je 0x140024e6c",
    0x24E19: "mov edx, dword ptr [rip + 0x24b485]",
    0x24E2B: "cmovle eax, edx",
    0x24E37: "imul edx, dword ptr [rip + 0x232fbe]",
    0x24E4D: "imul eax, dword ptr [rip + 0x232fa4]",
    0x24E6C: "mov eax, dword ptr [rip + 0x232f8a]",
    0x24E89: "divsd xmm0, qword ptr [rip + 0x24472f]",
    0x24E91: "call 0x1401d87e0",
    0x24EA2: "mov eax, dword ptr [rip + 0x232f50]",
    0x24EBF: "divsd xmm0, qword ptr [rip + 0x244701]",
    0x24EC7: "call 0x1401d87e0",
    0x24EE0: "call qword ptr [rip + 0x1da80a]",
    0x24EEE: "call qword ptr [rip + 0x1da804]",
    0x24F0B: "ret",
    0x17D7F1: "call 0x14017d680",
    0x17D81F: "jbe 0x14017d828",
    0x17D824: "mov r11d, dword ptr [rdx + rax*4]",
    0x461A4: "mov r8, rbx",
    0x461A7: "call 0x140024d20",
    0x461AC: "jmp 0x1400463ce",
    0x463CE: "cmp byte ptr [rip + 0x223075], r15b",
    0x463D5: "je 0x140047d01",
    0x463EF: "jmp 0x1400458e0",
}
BRANCHES = (
    "3006:A0-zero-literal-XY",
    "3006:A0-nonzero-global-or-current-self-XY",
    "3006:scaled-clamped-client-position",
    "3006:raw-clamped-client-position",
    "3006:pf-zero-success-or-nonzero-refetch",
)
BRANCH_ACCEPTANCE = {branch: "W10-007" if index < 4 else "W10-008"
                     for index, branch in enumerate(BRANCHES)}
FEATURES = ("W10-007", "W10-008")


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
            raise ValueError(f"reviewed 3006 source changed: {name}")
    image = pefile.PE(str(RUNTIME))
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    observed = {}
    for lo, hi, expected_sha in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if hashlib.sha256(blob).hexdigest() != expected_sha:
            raise ValueError(f"3006 machine window changed at {lo:#x}")
        instructions = list(decoder.disasm(blob, base + lo))
        if sum(insn.size for insn in instructions) != hi - lo:
            raise ValueError(f"3006 machine window incomplete at {lo:#x}")
        records = []
        for insn in instructions:
            rva = insn.address - base
            actual = f"{insn.mnemonic} {insn.op_str}".strip()
            if rva in SITES:
                if actual != SITES[rva]:
                    raise ValueError(f"3006 instruction changed at {rva:#x}: {actual}")
                observed[rva] = actual
            records.append({"rva": f"0x{rva:x}", "instruction": actual,
                            "bytes": insn.bytes.hex()})
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected_sha, "instructions": records})
    if set(observed) != set(SITES):
        raise ValueError("3006 key instruction outside frozen windows")

    normalized = (3006 & 0x3FF) - (((3006 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x500, 0x47EAA, 12, 0x47D84, 0x461A4):
        raise ValueError("3006 opcode-to-consumer route changed")
    imports = {imp.address - base: (module.dll.decode(), imp.name.decode())
               for module in image.DIRECTORY_ENTRY_IMPORT for imp in module.imports if imp.name}
    if imports.get(0x1FF6F0) != ("USER32.dll", "ClientToScreen") or imports.get(0x1FF6F8) != (
            "USER32.dll", "SetCursorPos"):
        raise ValueError("3006 OS cursor imports changed")
    if struct.unpack("<II", image.get_data(0x257DF8, 8)) != (240, 320):
        raise ValueError("3006 game coordinate bounds changed")

    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3006")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3006"]
    if (items["C250921-03006"]["status"] != "closed"
            or any(items[key]["parent_id"] != "W10-001" or items[key]["status"] != "open"
                   for key in FEATURES)
            or "3006" in items["W10-006"]["completion_condition"].split(";")[0]
            or inventory["effective_branch_ids"] != ";".join(BRANCHES)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != {"3006:command", "3006:0:0", "3006:0:1"}
            or any(row["acceptance_ids"] != ("W10-007;W10-008" if row["ID"] == "3006:command"
                                                  else "W10-007") for row in candidates)):
        raise ValueError("3006 branch and acceptance mapping incomplete")

    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3006, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}"},
        "imports": {"ClientToScreen": "USER32.dll IAT RVA 0x1ff6f0",
                    "SetCursorPos": "USER32.dll IAT RVA 0x1ff6f8"},
        "effective_branch_ids": list(BRANCHES),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(FEATURES),
        "operand_rule": "A0 exactly zero uses A1 and A2 as signed literal X/Y; every nonzero A0 uses A1 and A2 as one-lookup signed variable addresses. Negative addresses use current-self helper 17d7c0; zero or out-of-range positive addresses read zero. Missing A0/A1/A2 yields zero on the corresponding path. X is resolved before Y; alias and side-effect details remain dynamic acceptance.",
        "coordinate_rule": "With fewer than four Args or A3 bit 0 clear, clamp game X to [0,319] and Y to [0,239], divide by factors 2695c0/2695c8 and round through helper 1d87e0. With A3 bit 0 set, clamp to [0,S*320-1] and [0,S*240-1] where S is 2702a4 clamped to [1,4]; no factor division. Then call ClientToScreen(window 269638, point) and SetCursorPos(X,Y). Return values of those OS calls are ignored. Factor initialization is separately pinned by the 3005 scope audit.",
        "record_rule": "No minimum Args count: 0/1/2 count defaults missing values to zero; A3 is read only at count >=4. Other A3 bits, extra Args, Text and indent are not consumed by the opcode-specific runtime path and must remain intact. Formatter modes 0/1 correspond only to literal versus variable A0; runtime variable branch accepts every nonzero A0.",
        "schedule_rule": "After the consumer returns, shared dispatcher gate 463ce returns success when pf byte 26944a is zero and refetches the next record within the same invocation when nonzero on a valid frame. 3006 itself has no wait or map-event refresh call.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "context_evidence": "There is no 3006-specific context guard between shared selector and consumer. All four are static shared-dispatch candidates; no original/Player execution comparison is claimed.",
        "remaining_questions": [
            "W10-007: original/Player literal and every-nonzero A0 global/current-self/missing/out-of-range lookup; count 0..4/extra, alias/INT_MIN boundaries, raw/scaled clamps and factors, actual cursor in DPI/black-bar/focus states, OS failure/unsupported-platform policy and record retention.",
            "W10-008: pf zero/nonzero command budget and continuation in all applicable event contexts, including saved cursor/restart; no 3006-specific refresh.",
            "W10-006 retains other Windows input/console commands; 241028, Web and Android remain separate.",
        ],
        "scope": "250921 Windows static Set Mouse Position scope only; Player still warns and returns, so no feature parity is accepted.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: 3006 branches mapped to two open feature leaves")


if __name__ == "__main__":
    main()
