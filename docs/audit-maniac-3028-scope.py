"""Pin Windows 250921 Draw Tile On Picture scope; no Player parity claim."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3028-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
CONTRACT_SHA = "fde517473e883df2ac56aa7fdd3e18b11ffa3feeb47144c34a1c1de8e423bb1a"
REPORT = "analysis-work/reports/03028-5bdd7d63fbbe5c99b399d23cf125689982ae43e2fa54a75c66fa2b2d26bf0c79.json"
SOURCES = {
    "commands/03028.md": "ca8fe05917086d27677cb19ac98916bf066d2c2c29028d951122282794538329",
    "evidence/runtime-250921/000f1f70.c": "66c8d63eb41008b0bef16a6ef32cd05f7bccb77efad9ccaf533794c5efd6f2f0",
    "evidence/native-250921/00036170.c": "22f9b1d7b8b5ccd7443c3c256d1588fcb6df7918bbc4c1ec938934db7aeca1ce",
    "../Maniacs-250921/evidence/250921/DrawTileControl.txt": "cac7cfd0aa880b0a33b0c3d605be2327ea4eaa581d2aac34eb543950427108d8",
    REPORT: "7a63560f191f4a3b1ef8269a895da047fcfd44e8ac92e8c0879fa09d975c763b",
}
WINDOWS = (
    (0xF1F70, 0xF2F22, "d0440652e531cd59332e377e9327ad360eb0e2992dfcddef6e15e7ad1cc15dab"),
    (0xF1CA0, 0xF1F6A, "ac1b9035788007cca845181954562c082d960a0753fe84bd68a0baa6b84633ce"),
    (0xF15B0, 0xF1C9C, "c0943aefb520a19107531ca0bf7934d88265874f78c16a9b1b7131b43c8429c7"),
    (0x7D350, 0x7D7F6, "4f00966377faf85c95dd363081b250f79431a9adf6be1a5119d533ee68cacb16"),
)
BRANCH_ACCEPTANCE = {
    "3028:operand-resolution-and-empty-dimensions": "W06-014;W06-018",
    "3028:invalid-picture-error": "W06-014;W06-018",
    "3028:single-lower-autotile": "W06-015",
    "3028:single-lower-direct": "W06-015",
    "3028:single-upper": "W06-015",
    "3028:range-lower-autotile": "W06-014;W06-015",
    "3028:range-lower-direct": "W06-014;W06-015",
    "3028:range-upper": "W06-014;W06-015",
    "3028:range-base-literal": "W06-014",
    "3028:range-base-variable": "W06-014",
    "3028:range-base-indirect": "W06-014",
    "3028:temporary-autotile-neighbors-and-water-corners": "W06-015",
    "3028:tileset-current": "W06-016",
    "3028:tileset-replacement-load-release": "W06-016;W06-018",
    "3028:pattern-global-clock": "W06-016",
    "3028:pattern-explicit-and-negative": "W06-016",
    "3028:image-backing-detach-and-missing-storage": "W06-017;W06-018",
    "3028:clear-before-frame-offset": "W06-017",
    "3028:draw-sheet-cell-and-dirty-state": "W06-017;W06-018",
    "3028:short-record-text-and-direct-success": "W06-014;W06-018",
}
FEATURES = tuple(f"W06-{index:03d}" for index in range(14, 19))


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
            raise ValueError(f"reviewed 3028 source changed: {name}")
    catalog = json.loads((SOURCE / "reviewed-contracts.json").read_text(encoding="utf-8"))
    contract = next(row for row in catalog["commands"] if row["opcode"] == 3028)
    if sha(json.dumps(contract, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")) != CONTRACT_SHA:
        raise ValueError("reviewed 3028 contract changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    for lo, hi, expected in WINDOWS:
        if sha(image.get_data(lo, hi - lo)) != expected:
            raise ValueError(f"3028 machine window changed: {lo:#x}")
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected, "interpretation": "bounded bytes; code/table boundaries from pinned report"})
    # Check each acquired helper instruction and data probe against the binary,
    # without treating adjacent data or a bounded acquisition as executable code.
    report = json.loads((SOURCE / REPORT).read_text(encoding="utf-8"))
    instruction_count = 0
    for block in report["inputs"]["disassembly"]:
        if block["binary_sha256"] != RUNTIME_SHA:
            raise ValueError("3028 helper report references another binary")
        for instruction in block["instructions"]:
            blob = bytes.fromhex(instruction["bytes"])
            if image.get_data(int(instruction["rva"], 16), len(blob)) != blob:
                raise ValueError(f"3028 helper instruction changed: {instruction['rva']}")
            instruction_count += 1
    probes = []
    for probe in report["inputs"]["probes"]:
        if probe["request"]["image"] != "runtime":
            continue
        blob = bytes.fromhex(probe["hex"])
        rva = int(probe["request"]["rva"], 16)
        if image.get_data(rva, len(blob)) != blob:
            raise ValueError(f"3028 table probe changed: {rva:#x}")
        probes.append({"rva": f"0x{rva:x}", "size": len(blob), "sha256": sha(blob)})
    normalized = (3028 & 0x3FF) - (((3028 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x516, 0x47EC0, 32, 0x47DD4, 0x46910):
        raise ValueError("3028 opcode route changed")
    for rva, expected in ((0x46919, "call 0x1400f1f70"),
                          (0x4691E, "mov al, 1"), (0x46920, "jmp 0x140046ef3")):
        instruction = next(decoder.disasm(image.get_data(rva, 15),
                                          image.OPTIONAL_HEADER.ImageBase + rva))
        actual = f"{instruction.mnemonic} {instruction.op_str}"
        if actual != expected:
            raise ValueError(f"3028 key instruction changed: {rva:#x}: {actual}")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3028")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3028"]
    if (items["C250921-03028"]["status"] != "closed"
            or items["W06-001"]["item_kind"] != "summary"
            or any(items[key]["parent_id"] != "W06-001" or items[key]["result_type"] != "feature"
                   or items[key]["status"] != "open" for key in FEATURES)
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != {"3028:command"} | set(BRANCH_ACCEPTANCE)
            or any(row["acceptance_ids"] != (";".join(FEATURES) if row["ID"] == "3028:command"
                                                  else BRANCH_ACCEPTANCE[row["ID"]])
                   for row in candidates)):
        raise ValueError("3028 branch and acceptance mapping incomplete")
    result = {
        "runtime_sha256": RUNTIME_SHA, "source_sha256": SOURCES,
        "reviewed_contract_sha256": CONTRACT_SHA, "machine_windows": windows,
        "report_helper_instructions_verified": instruction_count, "table_probes": probes,
        "opcode_mapping": {"opcode": 3028, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}",
                           "branch_rva": f"0x{target:x}", "consumer_rva": "0xf1f70"},
        "effective_branch_ids": list(BRANCH_ACCEPTANCE),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE, "acceptance_ids": list(FEATURES),
        "operand_rule": "A0 eight nibbles resolve picture A1, pixel X/Y A2/A3, tile W/H A4/A5, source A6, tileset A10, time A11; modes0 literal,1 numeric,other indirect with negative/self propagation. Source value/base resolves after tileset/time and before nonpositive-size return; picture lookup follows size guard. A8/A9 whole nonzero select range/upper, unlike 3015 low bits.",
        "source_rule": "Single repeats one mapped source. Range base mode0 raw,1 one address lookup,other ca4f0 two reads; 8c800 exposes/grows W*H global/self numeric slots only once valid image storage reaches range generation. Negative bases traverse increasing magnitude. Temporary int32 tile rectangle is zero-initialized; lower unsigned0..17 maps fixed table,18..161 adds4982,other5143; upper0..143 adds10000,other10000.",
        "autotile_rule": "Only lower with A7 bit0 clear runs f1ca0 per row-major insertion, updating center mask255 then NW,N,NE,E,SE,S,SW,W masks4,140,8,25,1,35,2,70. f15b0 reads the temporary rectangle, not map neighbors. Classification, tileset neighbor mapping, old/new mask merge, shape lookup and water corners use the pinned reviewed contract and tables; future cells begin as zero.",
        "tileset_pattern_rule": "Resolved tileset<=0 or equal to current uses current object; other positive creates/loads/releases a temporary one. Time=-1 uses global clock; other signed time uses original truncating animation phases for internal tile<4000. Tiles>=4000 do not consume animation time. Missing tileset data and negative internal tile return in draw helper.",
        "image_rule": "Detach picture backing e2d70. Existing image storage triggers cached name release, state byte2 and dirty1. A7 bit1 clears [X,Y,X+16W,Y+16H] before sheet-frame offsets are added; drawing afterward uses frame offsets and 16-pixel steps. Missing image storage skips generation/drawing; invalid picture invokes Picture error. Dimension/product, allocation and sheet division unsafe boundaries remain explicit feature policy gaps.",
        "record_rule": "Missing A0..A11 numeric words zero; writer emits12 untrimmed words with empty Text, new editor default is only seven words. Preserve original count, unused Text, unknown option bits and tail; no image filename input.",
        "schedule_rule": "Direct success return after handler, no shared pf refetch or opcode-owned wait/explicit map refresh. Static contexts are map autostart/parallel, called common event and battle page; dynamic rendering, pending resources, save and continuation remain open.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "remaining_questions": [
            "W06-014: all eight modes, signed/self range bases, resolution order, zero slots and short records.",
            "W06-015: all six source/layer/autotile combinations, conversion boundaries and temporary neighbor/water shape results.",
            "W06-016: current/replacement tilesets, graphic load/release and signed animation pattern phases.",
            "W06-017: pixel mutation, backing detachment, resource names/dirty state, clear-before-frame and rendered output.",
            "W06-018: missing/invalid resource boundaries, safe arithmetic, four contexts, direct success and saved continuation.",
            "Player has no 3028 dispatch. C241028-03028, Web/Android and remaining W06-007 picture scope stay open.",
        ],
        "scope": "250921 Windows static 3028 command scope only; Player functionality and platform parity not accepted.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(BRANCH_ACCEPTANCE)} 3028 branches mapped to five open W06 leaves")


if __name__ == "__main__":
    main()
