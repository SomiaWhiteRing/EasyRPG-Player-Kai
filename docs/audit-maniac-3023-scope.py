"""Bind standalone Reserved1 dispatch, distinct from its editor formatter."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3023-scope-audit.json"
EXPECTED_RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
REPORT = "analysis-work/reports/03023-52eb7a4db43ca5953522b95910be7cce2ce355f3413c0e42ca5ec9091c112af3.json"
SOURCE_HASHES = {
    "commands/03023.md": "631a565f0b5de8b1f1471d456f23da28b1b8cf7b4fbe0d7c3205f7c0683b7847",
    "evidence/ReservedValues-250921.md": "f3982e8c7429015a4100a8b2bdf9f6545d3a9313419e71a83b77aa0c1b98c907",
    "evidence/native-250921/0004c9a0.c": "993ff23936713babf5f06b3ff7f237c4c1e7ebf750e9744fe7c5dce796883aaf",
    REPORT: "8cf55e19337917a52921a427a21f8db76c70195f61ce995204932a83b64aad9e",
}
WINDOWS = (
    (0x458CC, 0x459BD, "eb06ee1765a32fcd169bbbfca8e5773cf9ac3ee13dc8d61bc8eecb6b11d64ec0"),
    (0x47D01, 0x47D08, "322d8ebe2d452fba731c2b3dab7bb9ab54eb1df7c8b0430b94694aba81863d7d"),
    (0x46EF3, 0x46F0B, "791db4af3f544d75ac7db467d5e3547966f81418e8ad7deaf51c847227819921"),
)
SITES = {
    0x458D9: "lea r9, [rip - 0x458e0]",  # image base
    0x45919: "mov dword ptr [r14 + 0x24], eax",  # advanced list cursor
    0x4591D: "mov edx, dword ptr [rbx]",  # current record opcode
    0x45921: "and edx, 0x3ff",
    0x45927: "shr rax, 8",
    0x4592B: "movzx ecx, al",
    0x4592E: "shl ecx, 3",
    0x45931: "sub edx, ecx",
    0x45933: "add edx, 0x19a",
    0x45992: "add edx, 0xfffffbb2",
    0x459A7: "movzx eax, byte ptr [r9 + rax + 0x47df8]",
    0x459B0: "mov ecx, dword ptr [r9 + rax*4 + 0x47d54]",
    0x459B8: "add rcx, r9",
    0x459BB: "jmp rcx",
    0x47D01: "mov al, 1",
    0x47D03: "jmp 0x140046ef3",
    0x46F0A: "ret",
}
BRANCH_ID = "3023:standalone-explicit-success-no-payload"
FEATURE_ID = "W02-032"
CANDIDATE_IDS = {"3023:command", *(f"3023:2:{mode}" for mode in ("1", "2", "4", "5", "other"))}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_rows(path: Path, delimiter: str = ",") -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def main() -> None:
    if sha256(RUNTIME) != EXPECTED_RUNTIME_SHA:
        raise ValueError("fixed original runtime changed")
    for name, expected in SOURCE_HASHES.items():
        if sha256(SOURCE / name) != expected:
            raise ValueError(f"reviewed Reserved1 source changed: {name}")
    report = json.loads((SOURCE / REPORT).read_text(encoding="utf-8"))
    if not any(entry.get("dispatch", {}).get("opcode") == 3023
               and entry["dispatch"].get("branch_rva") == "0x47d01"
               and entry["dispatch"].get("stop_rva") == "0x46ef3"
               for entry in report["inputs"]["deferred"]):
        raise ValueError("reviewed 3023 opcode-to-exit mapping changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    base = image.OPTIONAL_HEADER.ImageBase
    windows = []
    actual_sites = {}
    for lo, hi, expected_sha in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if hashlib.sha256(blob).hexdigest() != expected_sha:
            raise ValueError(f"dispatch chain window changed at {lo:#x}")
        instructions = list(decoder.disasm(blob, base + lo))
        if sum(instruction.size for instruction in instructions) != hi - lo:
            raise ValueError(f"dispatch chain window did not decode completely at {lo:#x}")
        records = []
        for instruction in instructions:
            rva = instruction.address - base
            actual = f"{instruction.mnemonic} {instruction.op_str}".strip()
            if rva in SITES:
                if actual != SITES[rva]:
                    raise ValueError(f"opcode selection changed at {rva:#x}")
                actual_sites[rva] = actual
            records.append({"rva": f"0x{rva:x}", "instruction": actual,
                            "bytes": instruction.bytes.hex()})
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected_sha, "instructions": records})
    if set(actual_sites) != set(SITES):
        raise ValueError("opcode dispatch chain sites incomplete")
    normalized = (3023 & 0x3FF) - (((3023 >> 8) & 0xFF) << 3) + 0x19A
    # All prior range comparisons reject their other branch for this value;
    # the final upper-bound branches also reject their alternatives.
    if not (0x444 < normalized < 0x56F):
        raise ValueError("3023 no longer selects this dispatcher table")
    selector_rva = 0x47DF8 + normalized - 0x44E
    selector = image.get_data(selector_rva, 1)[0]
    target_rva = 0x47D54 + selector * 4
    target = struct.unpack("<I", image.get_data(target_rva, 4))[0]
    if (normalized, selector_rva, selector, target_rva, target) != (0x511, 0x47EBB, 29, 0x47DC8, 0x47D01):
        raise ValueError("3023 table mapping changed")
    branch_report = report["inputs"]["disassembly"][0]
    if (branch_report["binary_sha256"] != EXPECTED_RUNTIME_SHA
            or [(entry["rva"], entry["instruction"]) for entry in branch_report["instructions"]]
            != [("0x47d01", SITES[0x47D01]), ("0x47d03", SITES[0x47D03])]):
        raise ValueError("reviewed success branch differs from runtime")

    items = {row["id"]: row for row in read_rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in read_rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3023")
    candidates = [row for row in read_rows(ROOT / "docs/maniac-submode-ledger.csv") if row["opcode"] == "3023"]
    if (items[FEATURE_ID]["parent_id"] != "W02-008" or items[FEATURE_ID]["result_type"] != "feature"
            or inventory["effective_branch_ids"] != BRANCH_ID or inventory["acceptance_ids"] != FEATURE_ID
            or {row["ID"] for row in candidates} != CANDIDATE_IDS
            or any(row["acceptance_ids"] != FEATURE_ID for row in candidates)):
        raise ValueError("3023 branch and acceptance mapping incomplete")
    for row in candidates:
        if row["ID"] != "3023:command" and row["gap_category"] != "formatter-only; not an effective runtime branch":
            raise ValueError(f"formatter mode still presented as runtime scope: {row['ID']}")
    result = {
        "runtime_sha256": EXPECTED_RUNTIME_SHA,
        "source_sha256": SOURCE_HASHES,
        "machine_windows": windows,
        "opcode_mapping": {"opcode": 3023, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "selector_byte": selector,
                           "target_table_rva": f"0x{target_rva:x}", "target_rva": f"0x{target:x}"},
        "effective_branch_ids": [BRANCH_ID],
        "acceptance_ids": [FEATURE_ID],
        "candidate_dispositions": {row["ID"]: "standalone runtime branch" if row["ID"] == "3023:command"
                                   else "formatter structure only; no runtime destination-type mode"
                                   for row in candidates},
        "runtime_rule": "Shared fetch advances the list cursor, reads record opcode, computes selector 0x511, then byte entry 0x47ebb selects target-table slot 29 at 0x47dc8, whose RVA is 0x47d01. The complete target block sets AL=1 and jumps to the common epilogue ending in RET. No 3023-specific guard, call, Args or Text read lies on this path. Generic pre-dispatch wait/frame guards are shared mechanism scope.",
        "record_rule": "Standalone runtime imposes no Args minimum count, reads no destination/reference modes and consumes no counted literal Text segments. Empty, short, long and malformed payloads all take the same success branch; original opcode, indent, Args count/words and Text are retained for serialization and parent scans. Formatter field/length rules are editor evidence only.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "context_evidence": "The opcode-selection chain has no 3023 mode or scenario gate. All four contexts are static shared-dispatch candidates; this audit contains no game execution or original/Player timing comparison.",
        "remaining_questions": [
            "W02-032: fixed original/Player four-context success-return and continuation/budget parity, zero/short/long/malformed Args and arbitrary Text/indent retention, no value/reference effects, and saved cursor continuation after restart.",
            "241028 consumer decisions and Web/Android acceptance remain independent.",
        ],
        "scope": "250921 Windows standalone Reserved1 branch scope only; no formatter implementation or Player functionality is counted as complete.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: opcode 3023 selects success slot 29; five formatter modes separated")


if __name__ == "__main__":
    main()
