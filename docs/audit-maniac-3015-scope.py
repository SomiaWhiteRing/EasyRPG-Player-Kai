"""Pin Windows 250921 Rewrite Map scope and branch acceptance links."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3015-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCES = {
    "commands/03015.md": "660ec22f71ac3930a6fda4cfed5730cfb2d504033c87495b686afae950fad181",
    "evidence/NativeBatch06-250921.md": "28e131cfa55e8d1a4d24decb96d2eb2e56ebcb80f4b775a8104cd15412ac005f",
    "evidence/runtime-250921/00041a10.c": "486bd9dc0cf133a407e6a14db588624f91372f77ba7f334ddd1eb23f2cff66a5",
    "evidence/editor-250921/00304f04.c": "98468b59d832511276f873334fae02fc2c0194187e2f1100c7dab5eb6a581127",
    "evidence/native-250921/000455d0.c": "9aeb564276ea394a3de7103552f0a59c61e38475dcfe07036bf45f9d547d5cd3",
    "analysis-work/reports/03015-13d3b475acb8f53ca1f32c0b587dffce14748c576a05fea9a762b036f541d5fd.json": "04aed1c3ab97a535cd1b562507b35ef964fdb48a470d474aef8c39647458d4d3",
}
WINDOWS = (
    (0x468E3, 0x468F6, "5c4872b54a24617ef3b9857b05b8efbd5de8957c446b0e592180fae62c79109e", False),
    (0x41A10, 0x42756, "46bbbeb241fc95d98c5185b19c835dd889903aef28f834e490d976cf12fe8b18", False),
    (0x41880, 0x41A04, "74228e79e2b8715e905866c50b5ea2a8b972f935071cd2d84924a6b27aca58b2", False),
    (0x41440, 0x41878, "b9b122a2ae87fe10f828cdede3598dd6a8a696eb39c4bf7be942250a9380f255", False),
)
BRANCH_ACCEPTANCE = {
    "3015:operand-order-and-empty-size": "W05-011;W05-015",
    "3015:source-single-lower-autotile": "W05-011;W05-012;W05-014",
    "3015:source-single-lower-direct-y-defect": "W05-011;W05-012;W05-013",
    "3015:source-single-upper": "W05-011;W05-012;W05-013",
    "3015:source-range-lower-autotile": "W05-011;W05-012;W05-014",
    "3015:source-range-lower-direct": "W05-011;W05-012;W05-013",
    "3015:source-range-upper": "W05-011;W05-012;W05-013",
    "3015:range-base-literal": "W05-011;W05-012",
    "3015:range-base-variable": "W05-011;W05-012",
    "3015:range-base-indirect": "W05-011;W05-012",
    "3015:lower-upper-conversion-boundaries": "W05-012;W05-013",
    "3015:looped-and-nonlooped-map-bounds": "W05-013;W05-014",
    "3015:autotile-center-and-eight-neighbors": "W05-014",
    "3015:short-record-text-context-and-continuation": "W05-015",
}
FEATURES = tuple(f"W05-{index:03d}" for index in range(11, 16))


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
            raise ValueError(f"reviewed 3015 source changed: {name}")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    for lo, hi, expected, code in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if sha(blob) != expected:
            raise ValueError(f"3015 machine window changed: {lo:#x}")
        if code and sum(insn.size for insn in decoder.disasm(
                blob, image.OPTIONAL_HEADER.ImageBase + lo)) != len(blob):
            raise ValueError(f"3015 incomplete code window: {lo:#x}")
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected, "decoded_as_code": code})
    normalized = (3015 & 0x3FF) - (((3015 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x509, 0x47EB3, 21, 0x47DA8, 0x468E3):
        raise ValueError("3015 opcode route changed")
    for rva, expected in ((0x468E6, "call 0x140041a10"),
                          (0x4262C, "mov ebx, dword ptr [rsp + 0xd8]"),
                          (0x426C2, "mov word ptr [rax + rcx*2], bx")):
        instruction = next(decoder.disasm(image.get_data(rva, 15),
                                          image.OPTIONAL_HEADER.ImageBase + rva))
        actual = f"{instruction.mnemonic} {instruction.op_str}"
        if actual != expected:
            raise ValueError(f"3015 key instruction changed: {rva:#x}: {actual}")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3015")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3015"]
    expected_ids = {"3015:command"} | set(BRANCH_ACCEPTANCE)
    if (items["C250921-03015"]["status"] != "closed"
            or items["W05-010"]["item_kind"] != "summary"
            or any(items[key]["parent_id"] != "W05-010"
                   or items[key]["result_type"] != ("integration_platform" if key == "W05-015" else "feature")
                   or items[key]["status"] != "open" for key in FEATURES)
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != expected_ids
            or any(row["acceptance_ids"] != (";".join(FEATURES) if row["ID"] == "3015:command"
                                                  else BRANCH_ACCEPTANCE[row["ID"]])
                   for row in candidates)):
        raise ValueError("3015 branch and acceptance mapping incomplete")
    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3015, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}",
                           "branch_rva": f"0x{target:x}", "consumer_rva": "0x41a10"},
        "effective_branch_ids": list(BRANCH_ACCEPTANCE),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(FEATURES),
        "operand_rule": "Resolve X/Y/W/H from A4..A7 using A0 nibbles1..4 before source A3. Modes 0 literal,1 numeric read,other two reads; negative first reference forces self address. W<=0 or H<=0 returns before source resolution. A1 and A2 use bit0, not whole nonzero. A8 whole nonzero disables lower autotile.",
        "source_rule": "Single source evaluates A3 as one tile value with A0 nibble0. Range source treats A3 as variable base: raw, one read with negative propagation, or two reads; per-cell variable read is immediate, row-major, increasing absolute ID in selected bank. Missing/out-of-range source reads zero even when destination cell is skipped.",
        "write_rule": "Lower and upper tile values use distinct conversion tables and uint16 map storage. Each cell wraps in looped axes then tests bounds; out-of-range nonlooped writes skip. Lower A8=0 calls center-plus-eight-neighbor autotile helper per insertion, possibly outside rectangle. Lower A8!=0 writes directly. Machine-confirmed single+lower+A8!=0 defect writes uint16(startY) rather than converted tile; upper and lower range are unaffected.",
        "record_rule": "Native writer emits nine words and empty Text but runtime accepts short records with missing numeric values zero; preserve Args length, high bits, unknown tail and unused Text. No map ID or replacement tileset argument.",
        "schedule_rule": "Direct dispatcher calls handler and returns success; no opcode-owned wait, output variable, picture draw or explicit map-event refresh. Static contexts are map autostart/parallel, called common event and battle page. Subsequent tile consumers, rendering and persistence require dynamic acceptance.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "remaining_questions": [
            "W05-011: source/coordinate modes, negative self/indirect propagation, bit selection, evaluation order and nonpositive-size early return.",
            "W05-012: single and range source values, row-major read timing, missing variables and lower/upper conversion edges.",
            "W05-013: direct writes, looped/nonlooped bounds, original lower-single startY defect and safe int32/zero-dimension behavior.",
            "W05-014: center/eight-neighbor autotile masks, tile neighbors outside rectangle and consumer-visible updates.",
            "W05-015: short/extra records and Text, four contexts, direct-success scheduling, save/reload, scene and rendering integration.",
            "C241028-03015 and Web/Android remain separately scoped.",
        ],
        "scope": "250921 Windows static 3015 command scope only; Player functional differential remains open.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(BRANCH_ACCEPTANCE)} 3015 branches mapped to five open W05 leaves")


if __name__ == "__main__":
    main()
