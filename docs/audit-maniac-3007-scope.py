"""Pin 250921 Show String Picture's consumer and feature destinations."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3007-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCES = {
    "evidence/StringPicture-250921.md": "d8c80ade82ee04cb86a1875e5e2c33d0e0ea3646eee4b81330009bdf27ab3537",
    "evidence/runtime-250921/00036970.c": "4a86f5307f4754b84f6244b8712432763957f13d10b6478ba61610de7002d992",
    "evidence/runtime-250921/00035360.c": "48fccf72ceef02f9fdaafdd32e77d9f5be02756c618826577257cdb431646476",
    "evidence/runtime-250921/00033ee0.c": "914e81f5895a2a67b4de918f2341c1e4b5ad9f67b7c395fe9ada62192ea97b85",
    "evidence/runtime-250921/00034c60.c": "30542caa1aafd41e7be1e25d90c5915b6be1412294fb7003168f598e581ecc6e",
    "evidence/runtime-250921/00035d80.c": "26b67a777eb2084bd92eeec1db33d01e419725b678b37149e8f4b6a12bc748a6",
    "commands/03007.md": "0e8ab0758f874f454c5deb9d36fb6ac4889f24585e86c4c254e249db563e0f72",
}
WINDOWS = (
    (0x461B1, 0x461BE, "502aa718e3d66093f346a5cd2db4bdb1c0c3b87d0dbf13f3cfe0b69444108731"),
    (0x36970, 0x378C7, "d7f8d00bff80a312636117db20c92a08a34c9048e2d8b3ce1eedc0e3fac09692"),
    (0x463CE, 0x463F4, "3da0d6b60c895d16706be68bfa066eb223733d36324988829c76b03ba75d3c58"),
)
SITES = {
    0x461B1: "mov r8, rbx",
    0x461B4: "call 0x140036970",
    0x461B9: "jmp 0x1400463ce",
    0x36986: "mov r9d, dword ptr [r8 + 0x20]",
    0x36992: "je 0x1400369aa",
    0x36B10: "cmp dword ptr [r13 + 0x20], 0x12",
    0x36B98: "cmp dword ptr [r13 + 0x20], 0x13",
    0x370FA: "cmp dword ptr [r13 + 0x20], 0xc",
    0x3711B: "cmp dword ptr [r13 + 0x20], 0xe",
    0x3734D: "cmp dword ptr [r13 + 0x20], 0x15",
    0x37441: "call 0x1401f7ac0",
    0x37594: "cmp dword ptr [r13 + 0x20], 0x16",
    0x375B6: "call 0x140035360",
    0x3765F: "cmp dword ptr [r13 + 0x20], 3",
    0x378B7: "ret",
    0x378C2: "call 0x1400c8f60",
    0x463CE: "cmp byte ptr [rip + 0x223075], r15b",
    0x463D5: "je 0x140047d01",
    0x463EF: "jmp 0x1400458e0",
}
BRANCHES = (
    "3007:picture-id-and-packed-reference-modes",
    "3007:position-scale-effect-style-and-extended-A23",
    "3007:body-reference-and-three-text-segments",
    "3007:generated-image-layout-and-shared-picture-lifecycle",
    "3007:pf-zero-success-or-nonzero-refetch",
)
BRANCH_ACCEPTANCE = {
    BRANCHES[0]: "W06-004", BRANCHES[1]: "W06-004",
    BRANCHES[2]: "W06-005", BRANCHES[3]: "W06-006",
    BRANCHES[4]: "W06-006",
}
FEATURES = ("W06-004", "W06-005", "W06-006")


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
            raise ValueError(f"reviewed 3007 source changed: {name}")
    image = pefile.PE(str(RUNTIME))
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    observed = {}
    for lo, hi, expected_sha in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if hashlib.sha256(blob).hexdigest() != expected_sha:
            raise ValueError(f"3007 machine window changed at {lo:#x}")
        instructions = list(decoder.disasm(blob, base + lo))
        if sum(insn.size for insn in instructions) != hi - lo:
            raise ValueError(f"3007 machine window incomplete at {lo:#x}")
        records = []
        for insn in instructions:
            rva = insn.address - base
            actual = f"{insn.mnemonic} {insn.op_str}".strip()
            if rva in SITES:
                if actual != SITES[rva]:
                    raise ValueError(f"3007 instruction changed at {rva:#x}: {actual}")
                observed[rva] = actual
            records.append({"rva": f"0x{rva:x}", "instruction": actual,
                            "bytes": insn.bytes.hex()})
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected_sha, "instructions": records})
    if set(observed) != set(SITES):
        raise ValueError("3007 key instruction outside frozen windows")
    normalized = (3007 & 0x3FF) - (((3007 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x501, 0x47EAB, 13, 0x47D88, 0x461B1):
        raise ValueError("3007 opcode-to-consumer route changed")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3007")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3007"]
    if (items["C250921-03007"]["status"] != "closed"
            or items["W06-001"]["item_kind"] != "summary"
            or items["W06-007"]["item_kind"] != "unexpanded"
            or items["W06-007"]["parent_id"] != "W06-001"
            or any(items[key]["parent_id"] != "W06-001" or items[key]["status"] != "open"
                   for key in FEATURES)
            or inventory["effective_branch_ids"] != ";".join(BRANCHES)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != {
                "3007:command", "3007:10:0", "3007:10:1", "3007:10:2", "3007:10:3"}
            or any(row["acceptance_ids"] != (
                ";".join(FEATURES) if row["ID"] == "3007:command" else "W06-004;W06-006")
                   for row in candidates)):
        raise ValueError("3007 branch and acceptance mapping incomplete")
    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3007, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}",
                           "consumer_rva": "0x36970"},
        "effective_branch_ids": list(BRANCHES),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(FEATURES),
        "operand_rule": "A0 packs signed literal, one-lookup and indirect numeric modes for picture ID, XY, width, transparency, window size, font and origin; A13 selects effect angle reference modes. The runtime reads through A23: optional A23 supplies independent height scale when A0 top nibble is nonzero, while zero inherits resolved width. Missing consumed words default to zero but the original count and extra words must be serialized unchanged. Invalid picture ID takes the original non-returning Picture error path.",
        "effect_rule": "A10 whole value selects normal 0, rotation 1, wave 2 and fixed-angle 3; unknown values stay raw. Fixed angle resolves numerator/denominator through A13 reference modes and floating remainder without a denominator-zero fallback. Style, blend/flip and lifecycle flags retain unknown bits and their original positions; A15/A16 both zero substitutes map layer 7.",
        "text_rule": "A22 may reference the whole body when the first UTF-16 Text unit is 2 or 3; otherwise the original text enters preparation unchanged. Preparation then parses body, system graphic and font name by a single cursor, not a blind split at every control unit. Native editor writes byte 1 + body + byte 1 + system + byte 1 + font, with 21/22 Args; extended A22/A23 runtime records must not be truncated. The source contract pins body escapes, configuration references, defaults, length conversion and generated layout, but actual font/GDI output remains dynamic acceptance.",
        "state_rule": "Consumer releases prior picture backing, writes picture properties, generates a text image via 0x35360 and its body/config/layout helpers, then sets XY, origin and dirty dimensions. It has no opcode-owned wait. Shared dispatcher 0x463ce returns success at pf=0 and refetches at pf!=0. Shared picture renderer, map/battle lifecycle, save/restart and exact visual output still require original/Player comparison.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "context_evidence": "There is no 3007-specific context guard between shared selector and consumer. Four contexts are static dispatcher candidates, not observed gameplay executions.",
        "remaining_questions": [
            "W06-004: original/Player all packed reference modes, A0/A13/A23 and short/extended records, picture ID errors, state fields, effect and unknown flag retention.",
            "W06-005: original/Player encoded bytes versus UTF-16 text preparation, whole-body references, three segment cursor, escapes, font/system defaults and measured visual output.",
            "W06-006: original/Player generated pixel/render state, layer/lifecycle/save restoration and pf scheduling across applicable contexts.",
            "W06-001 retains other picture work; 241028, Web and Android remain independent.",
        ],
        "scope": "250921 Windows static 3007 scope only. Existing Player handler and static contract are not dynamic feature acceptance.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: 3007 mapped to three open W06 feature leaves")


if __name__ == "__main__":
    main()
