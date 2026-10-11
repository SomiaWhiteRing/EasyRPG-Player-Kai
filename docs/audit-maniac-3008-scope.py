"""Pin Get Picture Info geometry, pixel and scheduling scope for 250921."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3008-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCES = {
    "commands/03008.md": "0b0a183af83e02b30b0dde92022a4def572e351d225ae4a78948f90c275805e3",
    "evidence/runtime-250921/0003de00.c": "603baf43055f182535db16f8d93b0694a3499086798e742115f41b1cb4de2560",
    "evidence/NativeBatch06-250921.md": "28e131cfa55e8d1a4d24decb96d2eb2e56ebcb80f4b775a8104cd15412ac005f",
    "analysis-work/reports/03008-82dacacf8e39b8211228debc1a03ffb89ddd846ee1fd3dbb5ea01f978c1b1a5e.json": "82af16a1e04081aea2779eb8806591b50a921467ce0d3fa85a264217354c3c29",
}
WINDOWS = (
    (0x461BE, 0x461CB, "3544e87d2fa5e740673a8ccc7f4fd1da459508fc015dee7b009b7a3cc5a74cb5"),
    (0x3DE00, 0x3E6BC, "1bd1ae00b4990b9733e0b0810d2eff9f08f9e3274e3e6730cba93423dd698cfa"),
    (0x6CC10, 0x6D461, "c5e74f65b0c6d24debc87b16b116d6393422f32a3c0797beddfbcef59d6208da"),
    (0x8C9E0, 0x8CA80, "e4f9b2e55bf6174587026b439ebd379f361a08a2f84e3143ee336daf707c6122"),
    (0x8C800, 0x8C9E0, "fb700b9ebce81ed308c1a16d6190abc000abf3a62ea32d677d3bffd014e2b7b8"),
    (0x463CE, 0x463F4, "3da0d6b60c895d16706be68bfa066eb223733d36324988829c76b03ba75d3c58"),
    (0x1D87E0, 0x1D880A, "74d0968705e7aaaddbda2aa6d8a1173e90661b1ffac4916fe2fec1947414585f"),
    (0x1E8C4C, 0x1E8CAD, "c4c3f3a523b3e9b657076e593e16050de11f300261b6af8a29b1756ce369536a"),
    (0x1EBE00, 0x1EBF2F, "18b7a6cbb52be4c0e351866a52b0a259a0cf2a0774968f178e0f24f1351bda77"),
    (0x1EDB51, 0x1EDB8A, "31aec01aa553c7624734c4d743f7bd130e395dbe6a0d20e5eb244626152f6214"),
)
SITES = {
    0x461C1: "call 0x14003de00",
    0x3DF3D: "jae 0x14003e6ac",
    0x3DF76: "cmp ecx, 3",
    0x3DF79: "jl 0x14003dfec",
    0x3DF98: "jne 0x14003e691",
    0x3DFEC: "cmp dword ptr [r15 + 0x78], ebx",
    0x3DFF0: "jg 0x14003df7b",
    0x3E47C: "call 0x14006cc10",
    0x3E491: "call 0x1401d87e0",
    0x3E611: "call 0x14008c9e0",
    0x3E62A: "call 0x14008c9e0",
    0x3E643: "call 0x14008c9e0",
    0x3E65C: "call 0x14008c9e0",
    0x3E684: "call 0x140061b20",
    0x3E6AB: "ret",
    0x3E6B6: "call 0x1400c8f60",
    0x6CC8B: "imul edx, r10d",
    0x6CCEB: "test r14d, r14d",
    0x6CCFD: "test ebx, ebx",
    0x6CD0E: "test edx, edx",
    0x6CD1D: "call 0x14008c800",
    0x6CD7E: "and cl, 1",
    0x6CD81: "test byte ptr [rsp + 0xe0], 2",
    0x6CDBF: "idiv dword ptr [r15 + 0xd8]",
    0x6CF66: "and eax, 0xffffff",
    0x6D45C: "jmp 0x14006cfbe",
    0x8C818: "call 0x14017d740",
    0x8C85D: "jmp 0x14008c900",
    0x8CA01: "call 0x14017d740",
    0x463CE: "cmp byte ptr [rip + 0x223075], r15b",
    0x463D5: "je 0x140047d01",
    0x463EF: "jmp 0x1400458e0",
    0x1D87EF: "call 0x1401e8c4c",
    0x1D8800: "call 0x1401ebe00",
    0x1EBE12: "call 0x1401edae8",
    0x1EDB54: "mov r10d, 0x6000",
    0x1EDB5F: "cmp eax, 0x2000",
    0x1EDB66: "cmp eax, 0x4000",
    0x1EDB70: "mov ecx, 0x300",
    0x1EDB7A: "mov ecx, 0x200",
    0x1EDB81: "mov ecx, 0x100",
}
BRANCH_ACCEPTANCE = {
    "3008:packed-picture-reference-and-invalid-id": "W06-008;W06-009",
    "3008:geometry-type0-base-size": "W06-008",
    "3008:geometry-type1-current-scale": "W06-008",
    "3008:geometry-type2-target-scale": "W06-008",
    "3008:inactive-and-unknown-type-boundaries": "W06-008;W06-009",
    "3008:pixel-rectangle-buffer-alpha-and-contiguous-output": "W06-009",
    "3008:geometry-refresh-and-shared-pf-continuation": "W06-010",
}
FEATURES = ("W06-008", "W06-009", "W06-010")
CANDIDATE_ACCEPTANCE = {
    "3008:command": ";".join(FEATURES),
    "3008:0:0": "W06-008;W06-009",
    "3008:0:1": "W06-008;W06-009",
    "3008:1:0": "W06-008;W06-010",
    "3008:1:1": "W06-008;W06-010",
    "3008:1:2": "W06-008;W06-010",
    "3008:1:3": "W06-009;W06-010",
}


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
            raise ValueError(f"reviewed 3008 source changed: {name}")
    image = pefile.PE(str(RUNTIME))
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    observed = {}
    for lo, hi, expected_sha in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if hashlib.sha256(blob).hexdigest() != expected_sha:
            raise ValueError(f"3008 machine window changed at {lo:#x}")
        instructions = list(decoder.disasm(blob, base + lo))
        if sum(insn.size for insn in instructions) != hi - lo:
            raise ValueError(f"3008 machine window incomplete at {lo:#x}")
        sites = []
        for insn in instructions:
            rva = insn.address - base
            actual = f"{insn.mnemonic} {insn.op_str}".strip()
            if rva in SITES:
                if actual != SITES[rva]:
                    raise ValueError(f"3008 instruction changed at {rva:#x}: {actual}")
                observed[rva] = actual
                sites.append({"rva": f"0x{rva:x}", "instruction": actual,
                              "bytes": insn.bytes.hex()})
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected_sha, "instruction_count": len(instructions),
                        "decoded_bytes": sum(insn.size for insn in instructions), "key_sites": sites})
    if set(observed) != set(SITES):
        raise ValueError("3008 key instruction outside frozen windows")
    normalized = (3008 & 0x3FF) - (((3008 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x502, 0x47EAC, 14, 0x47D8C, 0x461BE):
        raise ValueError("3008 opcode route changed")
    if struct.unpack("<d", image.get_data(0x232358, 8))[0] != 100.0:
        raise ValueError("geometry scale divisor changed")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3008")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3008"]
    if (items["C250921-03008"]["status"] != "closed"
            or items["W06-001"]["item_kind"] != "summary"
            or items["W06-007"]["status"] != "open"
            or any(items[key]["parent_id"] != "W06-001" or items[key]["status"] != "open"
                   for key in FEATURES)
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != set(CANDIDATE_ACCEPTANCE)
            or any(row["acceptance_ids"] != CANDIDATE_ACCEPTANCE[row["ID"]] for row in candidates)):
        raise ValueError("3008 branch and acceptance mapping incomplete")
    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3008, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}",
                           "consumer_rva": "0x3de00"},
        "effective_branch_ids": list(BRANCH_ACCEPTANCE),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(FEATURES),
        "reference_and_record_rule": "A0 nibble0 resolves A3 picture ID: 0 literal, 1 signed global/current-self variable, all other values indirect with negative first reference forcing the intermediate negative absolute. Validate unsigned(ID-1) against picture count before inspecting A1, so missing/invalid ID is an original non-returning Picture error even for unknown type. Missing consumed words default zero; Text is never read. Keep original Args count, unused A8 in geometry, extra words, Text and unknown packed bits.",
        "geometry_rule": "A1 signed<3 checks active width +78 first. Inactive picture initializes X/Y/W/H to zero and still writes, even for negative unknown A1; active unknown types and all unknown positive types >=4 return without writes/refresh. Type0 rounds current XY +10/+18 and uses raw frame size +e0/+e4; type1 rounds current XY and frame size times current double scale +20/+28 /100; type2 rounds target XY +60/+68 and frame size times integer target scale +ac/+b0 /100. Rounding is helper1d87e0 with MXCSR-dependent mode followed by cvttsd2si, not an assumed fixed rounding rule.",
        "format_and_write_rule": "Whole A2=1 subtracts signed-truncated W/2,H/2 from center; A2=2 then replaces W/H with left+W/top+H without minus1; other values preserve center/size. Raw signed A4..A7 go to 8c9e0 in X,Y,W,H order, including global slot0 and negative self, so alias destinations overwrite earlier results. At pf=0 geometry calls 61b20 for current map events; pf!=0 skips refresh.",
        "pixel_rule": "Type3 resolves A4 X, A5 Y, A6 W, A7 H, A8 signed base through A0 nibbles1..5 in order, then 6cc10 takes A2 low byte. Require positive W/H, positive int32(W*H) and nonempty clipped intersection before 8c800 destination allocation; failed tests leave all destinations unchanged. Otherwise output W*H row-major slots with requested stride W and zero outside padding. Base0 is global physical slot0; negative is self storage. Bit0 picks original versus existing processed buffer with sheet-frame offsets dependent on image+60; bit1 strips copied alpha via 0x00ffffff. Other bits ignored; padding remains zero. No scale/rotation transform or geometry-style refresh occurs. Inconsistent internal picture buffers and arithmetic overflow remain original unsafe boundaries for dynamic safe-policy acceptance.",
        "schedule_rule": "No opcode-owned wait. Consumer returns to shared 463ce gate: pf=0 success, pf!=0 same-invocation refetch. Player currently introduces async image-load repeat and only geometry/global assignments; those differences are not accepted by this scope audit.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "context_evidence": "No 3008-specific context guard between shared selector and consumer; these are static candidates, not observed executions.",
        "remaining_questions": [
            "W06-008: fixed original/Player picture references, types/format, active/inactive/unknown boundaries, rounding, raw signed destination order/alias and record retention.",
            "W06-009: fixed original/Player packed pixel operands, clip/no-write/padding, original/processed sheet frame buffers, alpha flags, contiguous slot0/self storage and unsafe boundary policy.",
            "W06-010: fixed original/Player geometry versus pixel refresh, pf branches, pending-image timing, command budget and saved continuation in applicable contexts.",
            "W06-007 retains other picture commands; 241028, Web and Android remain independent.",
        ],
        "scope": "250921 Windows 3008 static scope only; no Player feature, dynamic rendering or save/platform acceptance is claimed.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: 3008 branches mapped to three open W06 feature leaves")


if __name__ == "__main__":
    main()
