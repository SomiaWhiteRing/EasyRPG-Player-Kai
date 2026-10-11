"""Audit the fixed 250921 standalone Expression command dispatch scope."""

from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
import pefile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
REPORT = SOURCE / "analysis-work/reports/03022-5704ff205abdf41773b0b49abb2e49355ec35463c6618dc56b209586169fe5fb.json"
CONTRACT = SOURCE / "commands/03022.md"
PROTOCOL = SOURCE / "evidence/Expression-Runtime-250921.md"
FORMATTER = SOURCE / "evidence/native-250921/00049350.c"
OUTPUT = ROOT / "docs/evidence/maniac-250921-3022-scope-audit.json"
EXPECTED_RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
FLAG_RVA = 0x26944A
SOURCE_HASHES = {
    CONTRACT: "41fa2e91684ca374da8f46fdb5233d3ae4821de601b243817acf08350a669ecf",
    PROTOCOL: "9af4a8a7c580b26d4e2f63a19e77d4830131141a58fd51e15a8c5da2519132d2",
    FORMATTER: "5d66b47a0a0c6ec3aa85c40e7d393e1c2e59f543539f737b7bfe654bf3059007",
    REPORT: "95455f4dd4bc84a0fe982b504840800c81915bc1e8ee01639511f90c35ba3300",
}
BRANCH = {
    0x456A0: "xor r15d, r15d",
    0x45703: "xor r15d, r15d",
    0x4622B: "cmp byte ptr [rip + 0x223218], r15b",
    0x46232: "je 0x140047d01",
    0x46238: "jmp 0x1400463e2",
    0x463E2: "mov eax, dword ptr [r13]",
    0x463E6: "sub eax, 1",
    0x463E9: "js 0x140047d15",
    0x463EF: "jmp 0x1400458e0",
    0x47D01: "mov al, 1",
    0x47D03: "jmp 0x140046ef3",
}
FLAG_WRITES = {
    0x17BC38: "mov byte ptr [rip + 0xed80b], bpl",
    0x17BE33: "setne byte ptr [rip + 0xed610]",
    0x17C1F7: "setne byte ptr [rip + 0xed24c]",
}
FLAG_CONFIGURATION = {
    0x17BC0B: "xor ebp, ebp",
    0x17C10C: "lea r8, [rip + 0xab175]",
    0x17C118: "lea rcx, [rsp + 0x30]",
    0x17C11D: "call 0x1401d8b24",
    0x17C1CD: "lea r9, [rsp + 0x30]",
    0x17C1D2: "lea rcx, [rip + 0xb4fd7]",
    0x17C1D9: "xor r8d, r8d",
    0x17C1DC: "lea rdx, [rip + 0xb4ff5]",
    0x17C1E3: "call qword ptr [rip + 0x8303f]",
    0x17C1E9: "test eax, eax",
}
BRANCH_TO_ACCEPTANCE = {
    "3022:driver-zero-immediate-success": "W02-031",
    "3022:driver-nonzero-same-invocation-next-fetch": "W02-031",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    if sha256(RUNTIME) != EXPECTED_RUNTIME_SHA:
        raise ValueError("fixed original runtime changed")
    for path, expected in SOURCE_HASHES.items():
        if sha256(path) != expected:
            raise ValueError(f"reviewed expression source changed: {path}")
    report = json.loads(REPORT.read_text(encoding="utf-8"))
    if not any(row.get("dispatch", {}).get("opcode") == 3022
               and row["dispatch"].get("branch_rva") == "0x4622b"
               for row in report["inputs"]["deferred"]):
        raise ValueError("reviewed 3022 dispatch mapping changed")
    machine_window = report["inputs"]["disassembly"][0]
    if machine_window["binary_sha256"] != EXPECTED_RUNTIME_SHA:
        raise ValueError("3022 branch report points to another binary")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    sites = {}
    for rva, expected in {**BRANCH, **FLAG_WRITES, **FLAG_CONFIGURATION}.items():
        instruction = next(decoder.disasm(image.get_data(rva, 16), image.OPTIONAL_HEADER.ImageBase + rva,
                                          count=1))
        actual = f"{instruction.mnemonic} {instruction.op_str}"
        if actual != expected:
            raise ValueError(f"3022 machine instruction changed at {rva:#x}: {actual}")
        sites[f"0x{rva:x}"] = {"instruction": actual, "bytes": instruction.bytes.hex()}
    if (sum(len(bytes.fromhex(sites[f"0x{rva:x}"]["bytes"])) for rva in (0x4622B, 0x46232, 0x46238))
            != 0x12):
        raise ValueError("3022 branch window is not complete")
    if [(row["rva"], row["instruction"]) for row in machine_window["instructions"][:3]] != [
            (f"0x{rva:x}", BRANCH[rva]) for rva in (0x4622B, 0x46232, 0x46238)]:
        raise ValueError("3022 reviewed branch report differs from PE")
    for rva in (0x4622B, *FLAG_WRITES):
        instruction = next(decoder.disasm(image.get_data(rva, 16), image.OPTIONAL_HEADER.ImageBase + rva,
                                          count=1))
        if not any(operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP
                   and instruction.address + instruction.size + operand.mem.disp
                   == image.OPTIONAL_HEADER.ImageBase + FLAG_RVA for operand in instruction.operands):
            raise ValueError(f"driver flag reference changed at {rva:#x}")
    imports = {entry.address - image.OPTIONAL_HEADER.ImageBase: entry.name
               for library in image.DIRECTORY_ENTRY_IMPORT for entry in library.imports}
    if imports.get(0x17C1E3 + 6 + 0x8303F) != b"GetPrivateProfileIntW":
        raise ValueError("driver flag INI import changed")
    for rva, value in {0x2311B0: "RPG_RT", 0x2311D8: "pf", 0x227288: "/rpg_rt.ini"}.items():
        if image.get_data(rva, len(value.encode("utf-16-le")) + 2) != value.encode("utf-16-le") + b"\0\0":
            raise ValueError(f"driver flag configuration name changed at {rva:#x}")

    with (ROOT / "docs/maniac-acceptance-items.tsv").open(encoding="utf-8-sig", newline="") as stream:
        items = {row["id"]: row for row in csv.DictReader(stream, delimiter="\t")}
    with (ROOT / "docs/maniac-250921-command-inventory.csv").open(encoding="utf-8-sig", newline="") as stream:
        inventory = next(row for row in csv.DictReader(stream) if row["opcode"] == "3022")
    with (ROOT / "docs/maniac-submode-ledger.csv").open(encoding="utf-8-sig", newline="") as stream:
        candidates = [row for row in csv.DictReader(stream) if row["opcode"] == "3022"]
    if (items["W02-031"]["parent_id"] != "W02-008"
            or items["W02-031"]["result_type"] != "feature"
            or set(inventory["effective_branch_ids"].split(";")) != set(BRANCH_TO_ACCEPTANCE)
            or inventory["acceptance_ids"] != "W02-031"
            or len(candidates) != 1 or candidates[0]["acceptance_ids"] != "W02-031"):
        raise ValueError("3022 acceptance mapping incomplete")
    if not all(branch in candidates[0]["parameters_and_modes"] for branch in BRANCH_TO_ACCEPTANCE):
        raise ValueError("3022 candidate omits a runtime branch")
    audit = {
        "runtime_sha256": EXPECTED_RUNTIME_SHA,
        "reviewed_report_sha256": sha256(REPORT),
        "reviewed_contract_sha256": sha256(CONTRACT),
        "expression_protocol_sha256": sha256(PROTOCOL),
        "formatter_sha256": sha256(FORMATTER),
        "machine_sites": sites,
        "driver_flag_rva": f"0x{FLAG_RVA:x}",
        "driver_flag_write_sites": [f"0x{site:x}" for site in FLAG_WRITES],
        "effective_branch_ids": list(BRANCH_TO_ACCEPTANCE),
        "acceptance_ids": ["W02-031"],
        "runtime_rule": "The standalone opcode 3022 block compares driver byte 0x26944a with zero held in r15b. Zero jumps to 0x47d01 and returns success; nonzero jumps to 0x463e2, validates an active frame, and refetches at 0x458e0 within the same dispatcher invocation. Neither branch reads this record's Args or Text or invokes expression evaluator 0x57500.",
        "record_rule": "No runtime minimum argument count is imposed by this block. Zero, short, extended and malformed Args all choose the same flag-dependent branch; preserve the original decoded Args, Text and indent for serialization and enclosing event scans. Formatter 0x49350 iterates expressions in Args for display only.",
        "context_rule": "The shared dispatcher has no opcode-3022 interpreter-mode guard. Map autostart, map parallel, called common event and battle event page are static candidate contexts. Both branches use a configuration byte, not an established map/battle-state selector. Original flag reachability and scheduling in each context await W02-031 differential evidence.",
        "flag_configuration": {"section": "RPG_RT", "key": "pf", "default": 0,
                               "ini_path_suffix": "/rpg_rt.ini", "conversion": "GetPrivateProfileIntW result != 0",
                               "initial_reset_site_rva": "0x17bc38", "ini_store_site_rva": "0x17c1f7",
                               "profile_store_site_rva": "0x17be33"},
        "flag_provenance_limit": "The INI read and boolean store are statically bound; a separate parsed-profile write also exists. Startup ordering, profile field identity/priority and externally reachable configuration combinations remain W02-031 questions. These limits do not add expression execution branches.",
        "expression_boundary": "Expression evaluation belongs to actual consumers, including 10220, 3019 argument mode 4 and 12010 predicate type 16, under W03/W02 acceptance. Opcode 3022's formatter/editor membership does not create an execution submode.",
        "remaining_questions": [
            "W02-031: establish flag zero/nonzero reachability in applicable original event contexts, same-invocation versus success-return timing, Player parity, and record retention with short/extended Args and Text.",
            "241028 consumer behavior and Web/Android scheduling remain independently open.",
        ],
        "scope": "Fixed 250921 Windows standalone opcode 3022 branch inventory only. No Player feature completion is claimed.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: two driver-flag scheduling branches; W02-031 remains open")


if __name__ == "__main__":
    main()
