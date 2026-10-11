"""Pin 250921 Control Shared Save routing, direct helpers and acceptance."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3016-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCES = {
    "commands/03016.md": "d71fa88402522fce657a959de3d9b7abb1fa3f48f384e4612d41a3e450b136e2",
    "evidence/runtime-250921/000431d0.c": "03a453c2f93ea90a0e1d380a596092a3a9a99c2a60a2820cb10a44ccef4d5da0",
    "evidence/native-250921/00045c20.c": "dad5298aa428dff7cfcdd47445d1942d12881e9cf50bfe94a5405439dfbf3770",
    "evidence/editor-250921/00305b8c.c": "7e92b1d64da02ca03b7795f6474f774ff1396ed0f6c42a0ed4b2392166578836",
    "evidence/NativeBatch05-250921.md": "d4fb9e8d0045a4849357118cd6351e3c5cb396ce1be4c48392070d13cb778faf",
}
WINDOWS = (
    (0x461DE, 0x461EB, "1dba3d56738497830664a19f6aed5195a6e7f57c0f4dcf891aa22c255a6f5d0b", True),
    (0x431D0, 0x43741, "29949ec2a0db78c86848a9254b58a9bde596c9e7a5dd3accc6fe35cad2ae3963", False),
    (0x42B80, 0x43068, "e7a2798570fb02718e90b68c7212cb7306308808c15d1176e3f33b400ce37f01", False),
    (0x43070, 0x431C4, "1da9c9f4e40c2302a7a80fe3833d1223f289ae9f7995d24f0c7b6d4ae3d7bb73", False),
    (0x42760, 0x42AE4, "989ec6a43d46406c39136825662e5ab97a18687ea50db48d769e6c8f3b2554ab", False),
    (0xDC880, 0xDCA0F, "843885af5c723ccffb1e4cae686a7b237f7db4461cf141c9f33f99dc26a80940", False),
    (0xDCA10, 0xDCB7A, "57de7a90e07eafb4b931f355fffc5af703d8c760c2b47d90c14c88886c36c662", False),
    (0x8D470, 0x8D59B, "60d6b7af233fffbf9040c255c2bcf60097febda7bb09001e440e1b31565e25ca", False),
    (0x8D5A0, 0x8D734, "5b2a8f9c613e3ce304cb298bfe4443fabdd7c83908fc86cd60228042850d3647", False),
    (0x463CE, 0x463F4, "3da0d6b60c895d16706be68bfa066eb223733d36324988829c76b03ba75d3c58", True),
)
BRANCH_ACCEPTANCE = {
    "3016:open-or-empty-lazy-load": "W11-030;W11-033;W11-034",
    "3016:close-discard": "W11-030;W11-034",
    "3016:save-if-open": "W11-030;W11-033;W11-034",
    "3016:save-then-close": "W11-030;W11-033;W11-034",
    "3016:shared-to-local-switch": "W11-031;W11-034",
    "3016:local-to-shared-switch": "W11-031;W11-034",
    "3016:shared-to-local-number": "W11-031;W11-034",
    "3016:local-to-shared-number": "W11-031;W11-034",
    "3016:shared-to-local-string": "W11-032;W11-034",
    "3016:local-to-shared-string": "W11-032;W11-034",
    "3016:unknown-operation-or-type": "W11-034",
    "3016:invalid-copy-guard-and-overflow": "W11-031;W11-032;W11-034",
}
FEATURES = ("W11-030", "W11-031", "W11-032", "W11-033", "W11-034")


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
            raise ValueError(f"reviewed 3016 source changed: {name}")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    for lo, hi, expected, code in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if sha(blob) != expected:
            raise ValueError(f"3016 machine window changed: {lo:#x}")
        if code and sum(insn.size for insn in decoder.disasm(
                blob, image.OPTIONAL_HEADER.ImageBase + lo)) != len(blob):
            raise ValueError(f"3016 code window incomplete: {lo:#x}")
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected, "decoded_as_code": code})
    normalized = (3016 & 0x3FF) - (((3016 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x50A, 0x47EB4, 22, 0x47DAC, 0x461DE):
        raise ValueError("3016 opcode route changed")
    for rva, expected in ((0x461E1, "call 0x1400431d0"),
                          (0x461E6, "jmp 0x1400463ce"),
                          (0x43212, "cmp eax, 5"),
                          (0x43215, "ja 0x1400435e0"),
                          (0x43239, "jmp 0x140043070"),
                          (0x43248, "jmp 0x140042760"),
                          (0x4324D, "call 0x140042760"),
                          (0x4325C, "jmp 0x140043070"),
                          (0x4327B, "call 0x140042b80")):
        instruction = next(decoder.disasm(image.get_data(rva, 15),
                                          image.OPTIONAL_HEADER.ImageBase + rva))
        actual = f"{instruction.mnemonic} {instruction.op_str}"
        if actual != expected:
            raise ValueError(f"3016 key instruction changed: {rva:#x}: {actual}")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3016")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3016"]
    expected_ids = {"3016:command"} | set(BRANCH_ACCEPTANCE)
    if (items["C250921-03016"]["status"] != "closed"
            or items["W11-029"]["item_kind"] != "summary"
            or any(items[key]["status"] != "open" or items[key]["parent_id"] != "W11-029"
                   or items[key]["result_type"] != ("integration_platform" if key == "W11-033" else "feature")
                   for key in FEATURES)
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != expected_ids
            or any(row["acceptance_ids"] != (";".join(FEATURES) if row["ID"] == "3016:command"
                                                  else BRANCH_ACCEPTANCE[row["ID"]])
                   for row in candidates)):
        raise ValueError("3016 branch and acceptance mapping incomplete")
    result = {
        "runtime_sha256": RUNTIME_SHA,
        "source_sha256": SOURCES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3016, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}",
                           "consumer_rva": "0x431d0", "shared_return_gate_rva": "0x463ce"},
        "effective_branch_ids": list(BRANCH_ACCEPTANCE),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(FEATURES),
        "operation_rule": "Whole A0 0 open, 1 close/discard, 2 save if open, 3 save then close, 4 shared-to-local copy, 5 local-to-shared copy; unknown returns. Empty record opens. Copy first lazy-opens. Opening sets flag before load; closing never saves; copying to shared never flushes. Type0 raw switches, type1 int32 numbers, type2 reference-counted UTF-16 strings; other type causes no copy.",
        "operand_rule": "A1 nibble0 resolves local A3, nibble1 shared A4, nibble2 count A5. Mode0 literal, mode1 one signed numeric lookup, other indirect; local mode1 negative first reference forces negative resolved base. Count resolves first, then local base, then shared base. A2 defaults zero. Positive count and invalid base on type2 can dereference null in original; int32 base+count and byte lengths can overflow.",
        "record_rule": "Writer emits one A0 for operation<=3, six words otherwise; runtime accepts all counts with missing words zero. No runtime Text or filename field read. Preserve extra Args, unknown A1 high bits, A5 extension, Text and indent.",
        "memory_rule": "Shared switch/numeric/string vectors retain reserved slot0. Close truncates all to one slot, releasing string references. Type0/1 copy requires positive count, nonzero local base and positive shared base; raw bytes or count*4 bytes copy without switch normalization. Type2 forward element assignment compares pointers, releases replaced references and retains sources.",
        "file_rule": "Save.lgs with LcfGlobalSave signature, tags1 switch bytes, 2 little-endian int32, optional3 UTF-16 strings and tag0 terminator. Save prefers existing SaveData directory; load prefers an existing SaveData/Save.lgs file, otherwise base path. Missing/invalid load leaves open flag set without retry until close. Unknown tags skip by stored length, malformed input is not transactional. Output-rate guard may prompt. No command-owned status destination or atomic backup.",
        "schedule_rule": "Direct 3016 call enters shared pf gate at 463ce: pf zero success, nonzero same-invocation refetch; no opcode-owned wait or map refresh. File I/O can invoke shared prompts. Four event contexts are static candidates until dynamic comparison.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "remaining_questions": [
            "W11-030: original/Player open/close/save/save-close transitions, unsaved changes, failure/retry and restart isolation.",
            "W11-031: type0/1 both copy directions, raw switch bytes, signed/self/invalid ranges, growth, alias/overflow and state consumers.",
            "W11-032: type2 both directions, null versus nonnull empty strings, reference counts, invalid-base defect and safe Player policy.",
            "W11-033: original/Player Save.lgs byte encoding, load/save path precedence, malformed files, cross-game isolation and platform file behavior.",
            "W11-034: A0..A5 short/extra records, mode nibbles/indirect/INT_MIN, resolution order, Text, contexts, pf and saved continuation.",
            "241028, Web and Android remain separately scoped; ordinary LSD W11-006 acceptance is distinct.",
        ],
        "scope": "250921 Windows static 3016 command scope only; Player shared-save behavior and original/Player dynamic parity remain open.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: 3016 branches mapped to five open W11 leaves")


if __name__ == "__main__":
    main()
