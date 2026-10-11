"""Pin 250921 Set Game Option routing and map its branches to open acceptance."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3018-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCES = {
    "commands/03018.md": "8a72734236e30055597b2ecaf1b113f9bfa6e35b5450e4b3b3ffe461eed11db7",
    "evidence/runtime-250921/00043cf0.c": "3515257a1839ecce639fe81a138297d92d21ff22bd827799e9fc23359020a53c",
    "evidence/native-250921/00046760.c": "3d0ea34e18b74f283118e684e6fd911dc85220c08ab9370bd4d1fadbc3952e86",
    "evidence/editor-250921/00306e84.c": "213904fa13b4053eca6ffe7914b904bd7e62ddc67b8ecdd97fac7c0c195721d7",
    "evidence/NativeBatch06-250921.md": "28e131cfa55e8d1a4d24decb96d2eb2e56ebcb80f4b775a8104cd15412ac005f",
    "evidence/BattleHooks-250921.md": "44cf863a91fa9953d27f6af6747555be3eee2c5e367a813c2647351e5e0ea3a2",
}
WINDOWS = (
    (0x461F8, 0x46205, "c7779fdc0616f3bb176e24718d7f276873c3996eae1e32ae6b7ee8bd71a2c5e5", True),
    (0x463CE, 0x463F4, "3da0d6b60c895d16706be68bfa066eb223733d36324988829c76b03ba75d3c58", True),
    (0x43CF0, 0x4463A, "282d9de47bb101de0e05fab1782ba3f45def084a10c9dba8ccd1919a2f6e59a1", False),
    (0x4463C, 0x4465C, "3a6cf773808ec15bf0f2f69236d305e62ec4cdeaba80b732171bb729f4519e75", False),
    (0x540F0, 0x54130, "eaf33ec88d56785297819f9074936ba303271c956dad0af9c2aed67a826883d4", False),
    (0x5AD90, 0x5B1BC, "7902b7027feef7df52fd0f928dae95a4a7720ce32235d1f75287c08f1274202d", False),
)
BRANCH_ACCEPTANCE = {
    "3018:short-count-inactive-clear": "W08-009;W08-012",
    "3018:option-0-inactive": "W08-009;W08-012",
    "3018:option-1-fps": "W08-009;W08-012",
    "3018:option-2-picture-limit": "W08-010;W08-012",
    "3018:option-3-frame-skip": "W08-009;W08-012",
    "3018:option-4-message-mouse": "W08-011;W08-012",
    "3018:option-5-battle-alignment": "W08-011;W08-012",
    "3018:option-6-animation-buffers": "W08-010;W08-012",
    "3018:option-7-face-cell": "W08-011;W08-012",
    "3018:unknown-option-frame-skip": "W08-009;W08-012",
}
FEATURES = ("W08-009", "W08-010", "W08-011", "W08-012")


def sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def rows(path: Path, delimiter: str = ",") -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def main() -> None:
    if sha(RUNTIME.read_bytes()) != RUNTIME_SHA:
        raise ValueError("fixed 250921 runtime changed")
    for name, expected in SOURCES.items():
        if sha((SOURCE / name).read_bytes()) != expected:
            raise ValueError(f"reviewed 3018 source changed: {name}")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    for lo, hi, expected, code in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if sha(blob) != expected:
            raise ValueError(f"3018 machine window changed: {lo:#x}")
        if code and sum(insn.size for insn in decoder.disasm(blob, image.OPTIONAL_HEADER.ImageBase + lo)) != len(blob):
            raise ValueError(f"3018 dispatch is not complete code: {lo:#x}")
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected, "decoded_as_code": code})
    normalized = (3018 & 0x3FF) - (((3018 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x50C, 0x47EB6, 24, 0x47DB4, 0x461F8):
        raise ValueError("3018 opcode route changed")
    branch_targets = [struct.unpack("<I", image.get_data(0x4463C + 4 * index, 4))[0]
                      for index in range(8)]
    if branch_targets != [0x43D40, 0x43D62, 0x43EC9, 0x445EB,
                          0x4400C, 0x44133, 0x4425A, 0x443DB]:
        raise ValueError("3018 option jump table changed")
    # Direct branch calls 43cf0 then reaches shared 463ce pf gate; unknown
    # options reach 445eb, the same setter as option 3.
    for rva, expected in ((0x461FB, "call 0x140043cf0"),
                          (0x46200, "jmp 0x1400463ce"),
                          (0x463D5, "je 0x140047d01"),
                          (0x43D23, "cmp ecx, 7"),
                          (0x43D26, "ja 0x1400445eb")):
        instruction = next(decoder.disasm(image.get_data(rva, 15),
                                          image.OPTIONAL_HEADER.ImageBase + rva))
        actual = f"{instruction.mnemonic} {instruction.op_str}"
        if actual != expected:
            raise ValueError(f"3018 key instruction changed: {rva:#x}: {actual}")

    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3018")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3018"]
    ids = {"3018:command"} | set(BRANCH_ACCEPTANCE)
    if (items["C250921-03018"]["status"] != "closed"
            or items["W08-001"]["item_kind"] != "summary"
            or any(items[key]["parent_id"] != "W08-001" or items[key]["result_type"] != "feature"
                   or items[key]["status"] != "open" for key in FEATURES)
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != ids
            or any(row["acceptance_ids"] != (";".join(FEATURES) if row["ID"] == "3018:command"
                                                  else BRANCH_ACCEPTANCE[row["ID"]])
                   for row in candidates)):
        raise ValueError("3018 branch and acceptance mapping incomplete")

    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3018, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}",
                           "consumer_rva": "0x43cf0", "return_gate_rva": "0x463ce"},
        "option_jump_table": {"rva": "0x4463c", "targets": [f"0x{value:x}" for value in branch_targets],
                              "unknown_option_target": "0x445eb"},
        "effective_branch_ids": list(BRANCH_ACCEPTANCE),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(FEATURES),
        "record_rule": "Count 0/1 clears inactive flag and returns; option 0 with missing A2 also clears it. Options 1/2/4/5/6 read A2 when count>=3; option 1 A3 when count>=4; option 7 reads A2/A3 with missing words zero. Extra Args, Text and indent have no command-specific reads and remain preserved.",
        "operand_rule": "For options 1/2/4/5/6, whole A0 is literal 0, signed global/current-self lookup 1, or indirect otherwise. Options 0 and 3/unknown use raw A2. Option 7 uses A0 low and next nibbles independently for width/height. Negative reference and INT_MIN/alias effects require dynamic validation.",
        "option_rule": "Options 0 inactive, 1 FPS, 2 picture limit, 3 frame skip, 4 message mouse, 5 battle alignment, 6 map-animation buffer count, 7 face cell size. Every other option uses the same raw A2 frame-skip setter as 3, not a no-op. Option 1 A3 packs test-play low nibble and right-shift skip bit4. FPS nonpositive raw request does not update actual rate but may reset timing. Picture limit has minimum 1 without 1000 cap; animation buffers clamp 1..1000 and free tail resources on shrink.",
        "consumers": "NativeBatch06-250921.md pins inactive timing/focus/audio; FPS frame duration and message RShift skip; picture and animation allocation; frame-skip render mask; message mouse input masks; battle alignment offsets; face cell rendering and layout. The 3038 type-1 zoom state is separate and is not produced by 3018.",
        "schedule_rule": "3018 consumer has no opcode-owned wait or refresh. Shared dispatch calls 43cf0 then enters 463ce pf gate: zero returns success, nonzero refetches per shared dispatcher. Four event contexts remain static candidates until dynamic comparison.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "remaining_questions": [
            "W08-009: inactive/focus/audio timing, FPS/test-play/skip-message, frame-skip masks and unknown-option fallback versus fixed original.",
            "W08-010: picture allocation and animation buffer shrink/grow, resource lifetime and saved/restart state.",
            "W08-011: message mouse, battle alignment and face-cell layout/render effects, including invalid sizes.",
            "W08-012: whole/nibble A0 source modes, signed/indirect/INT_MIN and alias cases, 0..4/extra Args and Text, four contexts and pf continuation.",
            "W08-013: other 3018-related time/hook behavior in W08-001 remains to be expanded; 241028/Web/Android remain separate.",
        ],
        "scope": "250921 Windows static 3018 command scope only; no Player feature, original/Player dynamic parity, saved continuation or platform behavior accepted.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: 3018 option branches mapped to four open W08 leaves")


if __name__ == "__main__":
    main()
