"""Pin Windows 250921 Get Game Info scope and branch acceptance links."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3021-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCES = {
    "commands/03021.md": "74800fc9338eca565680f9c0bd7d2ecafafb960a017c1cf894dfd1e5a480e9b4",
    "evidence/GetGameInfo-250921.md": "1b5d626b1318c2c0600e821295d01a1fc97bb64593c90e99abb77ed889db7f9f",
    "evidence/runtime-250921/00060720.c": "62f402e9950cedf110e0730e515f12806174e3ee069516f2ed4a4a3ea8b3100e",
    "evidence/native-250921/00047ca0.c": "c6afafe101eb7fb9350935949b2ba77993c31774b9fd7720e2bdc071ff8ef8c0",
    "analysis-work/reports/03021-fabf0d18a8ceea8655dd58dcb5e0a060fc34ef84ab73150b5312e7785e6708ca.json": "3633c12cbaf1fdf2a6561eec98325831abe433fb95cd73ed32208a70f25d75ad",
}
WINDOWS = (
    (0x46218, 0x4622B, "33d551b9a2dcb6a46ee99fb4cc977fc5a5f138c6f52f56c942709b4e14ed6e69", True),
    (0x60720, 0x61B1C, "facf761c2534d487c4e1c2f18347b105778ef153b75b421f84bb2ea29ba68f51", False),
    (0xC4A60, 0xC4AE6, "7eda6583bb5eb2bbdb284e6725b3a011620831ea650429e07c922b444f31d7f8", False),
    (0x1B21C0, 0x1B2240, "de5cb6043e31792da7498b682892db1e0f58e590c22887f86a1e8e8fa8ad0778", False),
    (0x6C5F0, 0x6C8F0, "c2cfdb4826217431d7e15a7332c6a95848945b7df20b2048fb881dfddc41850c", False),
)
OPERATION_TARGETS = (
    0x60788, 0x6079E, 0x60C34, 0x60C64, 0x6115E, 0x613BC,
    0x6180D, 0x613E2, 0x619B5, 0x619FD, 0x61A2D, 0x61AC7,
)
BRANCH_ACCEPTANCE = {
    "3021:op-0-map-size": "W05-004;W05-009",
    "3021:op-1-tile-rectangle": "W05-005;W05-009",
    "3021:op-2-window-size": "W05-004;W05-009",
    "3021:op-3-pixel-rectangle": "W05-006;W05-009",
    "3021:op-4-interpreter-state": "W05-007;W05-009",
    "3021:op-5-tileset-id": "W05-004;W05-009",
    "3021:op-6-face": "W05-008;W05-009",
    "3021:op-7-character-graphic": "W05-008;W05-009",
    "3021:op-8-camera": "W05-004;W05-009",
    "3021:op-9-shake": "W05-004;W05-009",
    "3021:op-10-bgm": "W05-009",
    "3021:op-11-and-unknown-noop": "W05-009",
    "3021:short-record-and-formatter-mismatch": "W05-005;W05-006;W05-009",
}
FEATURES = tuple(f"W05-{index:03d}" for index in range(4, 10))


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
            raise ValueError(f"reviewed 3021 source changed: {name}")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    for lo, hi, expected, code in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if sha(blob) != expected:
            raise ValueError(f"3021 machine window changed: {lo:#x}")
        if code and sum(insn.size for insn in decoder.disasm(
                blob, image.OPTIONAL_HEADER.ImageBase + lo)) != len(blob):
            raise ValueError(f"3021 dispatch is not complete code: {lo:#x}")
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected, "decoded_as_code": code})
    normalized = (3021 & 0x3FF) - (((3021 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x50F, 0x47EB9, 27, 0x47DC0, 0x46218):
        raise ValueError("3021 opcode route changed")
    table = tuple(struct.unpack("<i", image.get_data(0x61AEC + 4 * index, 4))[0]
                  for index in range(12))
    if table != OPERATION_TARGETS:
        raise ValueError("3021 operation jump table changed")
    for rva, expected in ((0x46221, "call 0x140060720"),
                          (0x46226, "jmp 0x1400463ce"),
                          (0x6076C, "cmp ecx, 0xb"),
                          (0x6076F, "ja 0x140061ac7")):
        instruction = next(decoder.disasm(image.get_data(rva, 15),
                                          image.OPTIONAL_HEADER.ImageBase + rva))
        actual = f"{instruction.mnemonic} {instruction.op_str}"
        if actual != expected:
            raise ValueError(f"3021 key instruction changed: {rva:#x}: {actual}")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3021")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3021"]
    expected_ids = {"3021:command"} | set(BRANCH_ACCEPTANCE)
    if (items["C250921-03021"]["status"] != "closed"
            or items["W05-003"]["item_kind"] != "summary"
            or items["W05-010"]["item_kind"] not in {"leaf", "summary"}
            or items["W05-010"]["status"] != "open"
            or any(items[key]["parent_id"] != "W05-003"
                   or items[key]["result_type"] != "feature"
                   or items[key]["status"] != "open" for key in FEATURES)
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != expected_ids
            or any(row["acceptance_ids"] != (";".join(FEATURES) if row["ID"] == "3021:command"
                                                  else BRANCH_ACCEPTANCE[row["ID"]])
                   for row in candidates)):
        raise ValueError("3021 branch and acceptance mapping incomplete")
    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3021, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}",
                           "branch_rva": f"0x{target:x}", "consumer_rva": "0x60720"},
        "operation_jump_table_rva": "0x61aec",
        "operation_targets": [f"0x{target:x}" for target in OPERATION_TARGETS],
        "effective_branch_ids": list(BRANCH_ACCEPTANCE),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(FEATURES),
        "record_rule": "A0 flags, A1 whole signed operation, A2..A7 branch inputs; missing numeric words zero. Zero/one-word records follow operation0 with destination0. Runtime never reads Text; preserve Text, original Args length, unknown bits and tail. Managed writer emits branch-specific 3/5/7/8 words with empty Text, but runtime accepts other lengths and has nibble disagreements for tile/pixel records.",
        "mode_rule": "Numeric mode0 literal,1 numeric variable,other indirect; negative first indirect reference forces negative second reference. Tile A3..A6 use A0 nibbles0..3; pixel A3..A7 use nibbles0..4; interpreter A2 destination nibble0 and A4 nest nibble2; face/graphic A5 subject nibble0. Do not substitute editor/formatter shifts.",
        "operation_rule": "0 map dimensions pair,1 lower/upper tile rectangle,2 window dimensions pair,3 framebuffer pixel rectangle,4 interpreter five-tuple,5 tileset scalar,6 face,7 character graphic,8 camera pair,9 shake pair,10 current BGM filename/settings; 11 and unknown operation return without command-specific mutation.",
        "destination_rule": "Pair/array helper can write numeric slot0 and signed self bank; scalar/string setters skip ID0. BGM string and four-numeric destinations are independently optional. Rectangles preserve row order, wrap/clip rules and 32-bit unsafe boundaries; pixel alpha is forced opaque or zero, not source alpha. Actor/event/vehicle graphic identities and fallback errors are branch-specific.",
        "schedule_rule": "Direct 3021 call enters shared pf gate: pf zero success, nonzero same-invocation refetch; no opcode-owned wait. Four event contexts are static candidates pending original/Player comparison. Frame nesting and saved continuation remain W05-007/W05-009 acceptance.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "remaining_questions": [
            "W05-004: scalar/pair outputs, signed self and zero slots, camera negative division and refresh.",
            "W05-005: tile runtime nibble0..3, map wrap, conversion, row layout and malformed dimensions.",
            "W05-006: pixel acquisition, alpha, stride, clipping/padding, unsafe rectangles and W06 rendering integration.",
            "W05-007: interpreter type/ID/five-tuple, positive/negative nest, invalid stack and W02 frame integration.",
            "W05-008: actor/message/event/player/vehicle face or graphic, override/fallback, bit0 option and missing-object errors.",
            "W05-009: BGM two optional destinations/four settings, short/extra Args, operation11/unknown, contexts/pf/save continuation.",
            "W05-010 retains 3015 RewriteMap under W05-003; C241028-03021 and Web/Android remain separate.",
        ],
        "scope": "250921 Windows static 3021 command scope only; no Player functional parity or old-version/platform result accepted.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(BRANCH_ACCEPTANCE)} 3021 branches mapped to six open W05 feature leaves")


if __name__ == "__main__":
    main()
