"""Pin 250921 Change Picture ID routing and branch acceptance."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3017-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCES = {
    "commands/03017.md": "ed533670b4066aaa50e7dad03ab339d21fea4c4245797c521d78a93a45f9bf67",
    "evidence/runtime-250921/00043760.c": "10acee5001082e5d7b4f8e4fc236f28ec40e0a306058d7b2b4042c3290386d6e",
    "evidence/native-250921/000464a0.c": "7163a675284ddc3fd84d71a7c5590aefce615b25cc368844e2f154594ea96f39",
    "evidence/editor-250921/003065a0.c": "33b1100c677dc67a74d3f9c0d1720e61e5a011899eb850e321311c350d897c87",
    "evidence/NativeBatch04-250921.md": "b5bdac61e7590d882f2b5901f5890134339dcf07515f5d3d23d38c036f2852b8",
}
WINDOWS = (
    (0x461EB, 0x461F8, "cf66b798de15eecfddd6de6f535ad51d4db589a8916438baf03ec175c66286b8", True),
    (0x43760, 0x43CEA, "0811fb6a1e60a125fe8a3b8b9b94bb1675d32db48f6f15783a6d187f6a409aa0", False),
    (0x6C4F0, 0x6C5EC, "93c75b55acd5566d1522069ac1497b0900637f4fe47d2fdae7a947d65533efec", False),
    (0x463CE, 0x463F4, "3da0d6b60c895d16706be68bfa066eb223733d36324988829c76b03ba75d3c58", True),
)
BRANCH_ACCEPTANCE = {
    "3017:count-nonpositive-or-same-start-noop": "W06-011;W06-013",
    "3017:move-ascending": "W06-011;W06-013",
    "3017:move-descending": "W06-011;W06-013",
    "3017:swap-ascending": "W06-011;W06-013",
    "3017:swap-descending": "W06-011;W06-013",
    "3017:slide-ascending": "W06-011;W06-013",
    "3017:slide-descending": "W06-011;W06-013",
    "3017:invalid-source-or-target": "W06-012;W06-013",
    "3017:erase-and-picture-object-id": "W06-011;W06-012",
}
FEATURES = ("W06-011", "W06-012", "W06-013")


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def rows(path: Path, delimiter: str = ",") -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def main() -> None:
    if sha(RUNTIME.read_bytes()) != RUNTIME_SHA:
        raise ValueError("fixed 250921 runtime changed")
    for name, expected in SOURCES.items():
        if sha((SOURCE / name).read_bytes()) != expected:
            raise ValueError(f"reviewed 3017 source changed: {name}")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    for lo, hi, expected, code in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if sha(blob) != expected:
            raise ValueError(f"3017 machine window changed: {lo:#x}")
        if code and sum(insn.size for insn in decoder.disasm(
                blob, image.OPTIONAL_HEADER.ImageBase + lo)) != len(blob):
            raise ValueError(f"3017 code window incomplete: {lo:#x}")
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected, "decoded_as_code": code})
    normalized = (3017 & 0x3FF) - (((3017 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x50B, 0x47EB5, 23, 0x47DB0, 0x461EB):
        raise ValueError("3017 opcode route changed")
    for rva, expected in ((0x461EE, "call 0x140043760"),
                          (0x461F3, "jmp 0x1400463ce"),
                          (0x463D5, "je 0x140047d01")):
        instruction = next(decoder.disasm(image.get_data(rva, 15),
                                          image.OPTIONAL_HEADER.ImageBase + rva))
        actual = f"{instruction.mnemonic} {instruction.op_str}"
        if actual != expected:
            raise ValueError(f"3017 key instruction changed: {rva:#x}: {actual}")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3017")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3017"]
    expected_ids = {"3017:command"} | set(BRANCH_ACCEPTANCE)
    if (items["C250921-03017"]["status"] != "closed"
            or any(items[key]["status"] != "open" or items[key]["result_type"] != "feature"
                   or items[key]["parent_id"] != "W06-001" for key in FEATURES)
            or items["W06-007"]["item_kind"] != "unexpanded"
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != expected_ids
            or any(row["acceptance_ids"] != (";".join(FEATURES) if row["ID"] == "3017:command"
                                                  else BRANCH_ACCEPTANCE[row["ID"]])
                   for row in candidates)):
        raise ValueError("3017 branch and acceptance mapping incomplete")
    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3017, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}",
                           "consumer_rva": "0x43760", "shared_return_gate_rva": "0x463ce"},
        "effective_branch_ids": list(BRANCH_ACCEPTANCE),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(FEATURES),
        "record_rule": "A0..A5 missing words default zero, including count and ignore-error bit. No runtime Text read. Original count, extra Args, unknown A1/A5 bits, Text and indent remain preserved; writer emits six args without Trim.",
        "operand_rule": "A1 low nibble resolves A2 source, next nibble resolves A3 count, whole bits8..15 resolve A4 target/slide offset (not only editor nibble2). For each: mode0 literal, mode1 signed global/current-self single lookup, other modes indirect. Negative references and INT_MIN/alias behavior remain for dynamic comparison.",
        "operation_rule": "A0 whole0 move, 1 swap, every other nonzero value slide by signed-int32 source+offset. Count<=0 or resolved target==source returns before range checks. Signed target>source traverses down, otherwise up. Each valid destination is erased before source validation except swap; valid source/target swap pointers and rewrite each picture object ID+0x80. Overlapping ranges mutate sequentially.",
        "error_and_erase_rule": "A5 bit0 suppresses Picture range errors; other bits unused. Invalid source may leave an already-erased valid destination; invalid destination may erase a valid source even in swap mode. Fixed helper 6c4f0 releases name/image/shared resources and resets active/timing/effects while retaining the object/slot. Unsafe original error paths require bounded comparison and documented safe Player policy.",
        "schedule_rule": "Shared dispatch calls 43760 then pf gate 463ce: pf zero success, nonzero same-invocation refetch. No opcode-owned wait or map-event refresh. Four event contexts are static candidates, not observed dynamic executions.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "remaining_questions": [
            "W06-011: Player move/swap/slide, ascending/descending overlap, pointer/object-ID and downstream image identity versus fixed original.",
            "W06-012: Player erase/resource release and invalid source/target with A5 ignore bit, partial mutations and safe error policy.",
            "W06-013: numeric/indirect/self source order, short/extra Args and Text, four contexts, pf scheduling and saved continuation.",
            "W06-007: other picture numbering, pixels, layers, scale, map/battle lifecycle and save restoration remain unexpanded; W11-005 dynamic presentation save remains separate.",
            "241028, Web and Android behavior remain separately scoped.",
        ],
        "scope": "250921 Windows static 3017 command scope only. No Player feature or original/Player dynamic parity is accepted.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: 3017 branches mapped to three open W06 leaves")


if __name__ == "__main__":
    main()
