"""Pin 250921 Control Media Option routing, consumer and 3006 scale link."""

from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86_const import X86_REG_RIP
import pefile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-3038-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCES = {
    "commands/03038.md": "cb29cff2b24d8e6aea4ea2078a51c5ffbd23b200bdf24e475e86e44d5b41488c",
    "evidence/runtime-250921/0004d570.c": "fd454c6da5c0bd3d4432b4846060495866cef7ebb8ba41cc21287cda89e943c8",
    "evidence/native-250921/0004c190.c": "11f459bf135144ce1d5bb5f3d635d7f79bf4f657aae0979586d0ef24d0a11bf5",
    "evidence/runtime-250921/00024d20.c": "e355ee1c8ea6c97d02921e3a1b7e2683afac49e56f6f4efc260fa5e6d52d0bbc",
}
WINDOWS = (
    (0x46A1E, 0x46A2D, "cddd57e302e52c420a545c676097c38d1cac0602f3e5b4f35301bfa157497c38", True),
    (0x4D570, 0x4DA0E, "c31be313effb0148745c47d4fc3b9095cc159ddc0b56300b31c822285583c837", True),
    (0x4DA10, 0x4DA60, "e633497b5db4bf2e39cd5b3954629ac12a1b9511b4970c270fe42b4095b1af9c", False),
    (0x24D20, 0x24F0C, "193aceef4817273116bc77365c4720aec35a7a308b316ee46f83039869b733e8", True),
)
SITES = {
    0x46A21: "call 0x14004d570",
    0x46A28: "jmp 0x140046ef3",
    0x4D57C: "mov edx, dword ptr [r8 + 0x20]",
    0x4D5AF: "and ecx, 0xf",
    0x4D768: "cmp ecx, 1",
    0x4D828: "test esi, esi",
    0x4D830: "cmp esi, 1",
    0x4D839: "cmp edi, 9",
    0x4D886: "mov dword ptr [rip + 0x222a18], eax",
    0x4D8A7: "mov dword ptr [rip + 0x2229f7], eax",
    0x4D982: "cmp edi, 9",
    0x4D9A6: "mov edx, dword ptr [rip + 0x2228f8]",
    0x4D9F3: "call 0x1400d9fd0",
    0x4D9FC: "call 0x14008c9e0",
    0x24E19: "mov edx, dword ptr [rip + 0x24b485]",
}
OPTION_NAMES = (
    "screen-mode", "window-zoom", "window-interpolation",
    "keep-window-in-display", "fullscreen-uneven-pixels",
    "fullscreen-ignore-aspect", "fullscreen-interpolation",
    "show-fps", "save-current-default", "audio-volume",
)
BRANCH_ACCEPTANCE = {
    f"3038:{operation}-option-{index}-{name}":
        ("W08-005;W08-008" if index == 1 else
         "W08-007;W08-008" if index == 9 else "W08-006;W08-008")
    for operation in ("get", "set")
    for index, name in enumerate(OPTION_NAMES)
}
BRANCH_ACCEPTANCE["3038:other-operation-or-option-noop"] = "W08-008"
FEATURES = ("W08-005", "W08-006", "W08-007", "W08-008")


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(path: Path, delimiter: str = ",") -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def main() -> None:
    if sha(RUNTIME) != RUNTIME_SHA:
        raise ValueError("fixed 250921 runtime changed")
    for name, expected in SOURCES.items():
        if sha(SOURCE / name) != expected:
            raise ValueError(f"reviewed 3038 source changed: {name}")
    image = pefile.PE(str(RUNTIME))
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    windows = []
    observed = {}
    for lo, hi, expected_sha, decode in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if sha_bytes(blob) != expected_sha:
            raise ValueError(f"3038 machine window changed at {lo:#x}")
        sites = []
        if decode:
            instructions = list(decoder.disasm(blob, base + lo))
            if sum(insn.size for insn in instructions) != len(blob):
                raise ValueError(f"3038 machine code incomplete at {lo:#x}")
            for insn in instructions:
                rva = insn.address - base
                actual = f"{insn.mnemonic} {insn.op_str}".strip()
                if rva in SITES:
                    if actual != SITES[rva]:
                        raise ValueError(f"3038 key instruction changed at {rva:#x}: {actual}")
                    observed[rva] = actual
                    sites.append({"rva": f"0x{rva:x}", "instruction": actual,
                                  "bytes": insn.bytes.hex()})
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected_sha, "decoded_as_code": decode, "key_sites": sites})
    if set(observed) != set(SITES):
        raise ValueError("3038 key instruction outside frozen windows")

    normalized = (3038 & 0x3FF) - (((3038 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x520, 0x47ECA, 39, 0x47DF0, 0x46A1E):
        raise ValueError("3038 opcode route changed")
    for table_rva, expected in (
            (0x4DA10, (0x4D858, 0x4D877, 0x4D8C3, 0x4D8D9, 0x4D8F8,
                       0x4D90E, 0x4D924, 0x4D93A, 0x4D959, 0x4D96F)),
            (0x4DA38, (0x4D99D, 0x4D9A6, 0x4D9AE, 0x4D9B7, 0x4D9C6,
                       0x4D9CF, 0x4D9D8, 0x4D9E1, 0x4D9EA, 0x4D9F3))):
        targets = tuple(struct.unpack("<i", image.get_data(table_rva + 4 * i, 4))[0]
                        for i in range(10))
        if targets != expected:
            raise ValueError(f"3038 option jump table changed at {table_rva:#x}")
    for rva in (0x4D886, 0x4D8A7, 0x4D9A6, 0x24E19):
        instruction = next(insn for insn in decoder.disasm(image.get_data(rva, 8), base + rva))
        # All four fixed RIP-relative instructions address the same zoom state.
        displacement = next(operand.mem.disp for operand in instruction.operands
                            if operand.type == 3 and operand.mem.base == X86_REG_RIP)
        if rva + instruction.size + displacement != 0x2702A4:
            raise ValueError(f"3038/3006 zoom state link changed at {rva:#x}")

    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3038")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3038"]
    expected_ids = {"3038:command"} | set(BRANCH_ACCEPTANCE)
    if (items["C250921-03038"]["status"] != "closed"
            or items["W08-004"]["item_kind"] != "summary"
            or any(items[key]["status"] != "open" or items[key]["result_type"] != "feature"
                   or items[key]["parent_id"] != "W08-004"
                   for key in FEATURES)
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != expected_ids
            or any(row["acceptance_ids"] != (";".join(FEATURES) if row["ID"] == "3038:command"
                                                  else BRANCH_ACCEPTANCE[row["ID"]]) for row in candidates)):
        raise ValueError("3038 branch and acceptance mapping incomplete")

    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3038, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}",
                           "consumer_rva": "0x4d570"},
        "effective_branch_ids": list(BRANCH_ACCEPTANCE),
        "option_jump_tables": {"set_rva": "0x4da10", "get_rva": "0x4da38",
                               "option_count_each": 10},
        "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(FEATURES),
        "operand_rule": "A0 nibbles 0,1,2 resolve A1 operation, A2 option type and A3 value/destination in order: mode 0 literal, 1 single global/current-self lookup, other nonzero modes indirect lookup. Missing words default zero. Get writes resolved signed A3 destination through 8c9e0 (zero skipped); set uses resolved A3 value. Formatter and managed editor constrain ordinary writer UI, not runtime modes. Indirection including negative references, INT_MIN and alias effects needs dynamic comparison.",
        "option_rule": "Resolved operation 0 reads option types 0..9; operation 1 sets types 0..9; other operations/types do no option action. Types 0..8 cover screen/window/fullscreen/FPS/default state. Boolean setters use nonzero; type 3 stores an inverted byte but returns the logical value. Type 1 zoom setter clamps signed input to [1,4] and invokes 0da130 then 53f00; get returns that state. Types 0,3,7 also invoke display callbacks; types 2,4,5,6,8 only store their byte. Type 9 goes through audio getter d9fd0 or setter d9e10; exact volume conversions and failure paths require feature validation.",
        "cursor_scale_link": "3038 set type 1 writes 2702a4 at RVA 4d886 or 4d8a7; get type 1 reads it at 4d9a6; 3006 raw-coordinate branch reads the same 2702a4 at 24e19, then clamps S to [1,4] for coordinate limits. This is 3038, not 3018 (3018 option 1 is FPS).",
        "record_rule": "No four-argument minimum: count 0 defaults A0/A1/A2/A3 to zero; count 1 defaults A1/A2/A3; count 2 defaults A2/A3; count 3 defaults A3. Other A0 bits, extra Args, Text and indent have no opcode-specific read and must remain intact.",
        "schedule_rule": "Dispatcher calls 4d570 and returns AL=1 via 46ef3; no opcode-owned wait or pf refresh/refetch. Option setters 0/1/3/7 call display helpers inside the consumer; these are option effects, not dispatcher scheduling.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "context_evidence": "No 3038-specific context guard between shared selector and consumer. These are static shared-dispatch candidates, not observed executions.",
        "remaining_questions": [
            "W08-005: original/Player type 1 zoom get/set, clamp, display callbacks and 3006 raw-coordinate consumer after zoom changes.",
            "W08-006: original/Player types 0 and 2..8 screen/window/fullscreen/FPS/default read/set and visible effects, callback timing, save/default and platform policies.",
            "W08-007: original/Player type 9 audio volume backend conversion, returned values, unavailable/failure behavior and actual output.",
            "W08-008: operation/type/value packed modes, short/extra Args and Text, global/self/indirect read and signed destination writes, unknown branches, alias order, context and continuation.",
            "W10-007: actual 3006 cursor parity after 3038 zoom changes; 241028, Web and Android remain separate.",
        ],
        "scope": "250921 Windows static 3038 command scope only; no Player 3038 implementation, visual/audio behavior or dynamic context acceptance is claimed.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: 3038 get/set option branches mapped to four open W08 leaves")


def sha_bytes(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


if __name__ == "__main__":
    main()
