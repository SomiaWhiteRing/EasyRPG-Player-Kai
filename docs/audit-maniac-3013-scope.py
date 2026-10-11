"""Bind the fixed 250921 Variable Array branch inventory to original machine code."""

from __future__ import annotations

import csv
import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-3013-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
REPORT = "analysis-work/reports/03013-fdc030ef232bf45d923ac72371dac3d95869b3ff46fca95b5c9d7387ee1e58f2.json"
SOURCE_HASHES = {
    "commands/03013.md": "a39d5e3327ee03138a71c17c4dab1883344bf3c25fe656cef6d9479831387dad",
    "evidence/VariableArray-250921.md": "c75b2151e0d412e82b906d095fa754f81681c26a947a7775356c68e26719f056",
    "evidence/runtime-250921/0003f620.c": "cc013ced6194d3dc7a2a78286406f0d6b372b888795d38fcd8ee80d93ac90025",
    REPORT: "5cc8bdc0dc45abb9eebe309f49ef681b6b12476433c6324a4f05f9fc4c0371c7",
}
WINDOWS = (
    (0x3F620, 0x404A1), (0x4A900, 0x4ABCC), (0x49E20, 0x4A0EC),
    (0x3F440, 0x3F530), (0x3F530, 0x3F620), (0x37BD0, 0x37CB8),
    (0x8C300, 0x8C3AA), (0x8C730, 0x8C800), (0xCA4F0, 0xCA589),
    (0x1F9AE0, 0x1FA14D), (0x4ABD0, 0x4AD3A), (0x4AD40, 0x4AEAA),
)
SITES = {
    0x3F62D: "mov edi, dword ptr [r8 + 0x20]",
    0x3F636: "cmp edi, 1",
    0x3F646: "and ecx, 0xf",
    0x3F658: "call 0x1400ca4f0",
    0x3F6CD: "shr ecx, 4",
    0x3F6D7: "je 0x14003f78e",
    0x3F6E0: "je 0x14003f766",
    0x40456: "cmp byte ptr [rip + 0x228fee], bl",
    0x40478: "call 0x140061b20",
}
BRANCH_TO_ACCEPTANCE = {
    "3013:base-literal-single-indirect-signed-self": "W03-010",
    "3013:count-and-operand-numeric-versus-address": "W03-010",
    "3013:zero-base-nonpositive-count-and-unknown-operation": "W03-010",
    "3013:copy-swap-reverse-and-overlap": "W03-011",
    "3013:single-and-paired-sort-signed-alias-and-negative-self": "W03-012",
    "3013:single-and-paired-shuffle-lcg-and-alias": "W03-013",
    "3013:enumerate-fill-and-numeric-wrap": "W03-014",
    "3013:arithmetic-bitwise-shifts-clamp-and-zero-divisor": "W03-015",
    "3013:dereference-signed-bank-polarity": "W03-016",
    "3013:refresh-driver-context-and-operation-exclusions": "W03-017",
}
OPERATION_TO_ACCEPTANCE = {
    **{op: "W03-011" for op in (0, 1, 21, 22)},
    **{op: "W03-012" for op in (2, 3, 17, 18)},
    **{op: "W03-013" for op in (4, 19)},
    **{op: "W03-014" for op in (5, 20)},
    **{op: "W03-015" for op in range(6, 16)},
    16: "W03-016",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_rows(path: Path, delimiter: str) -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def main() -> None:
    if digest(RUNTIME) != RUNTIME_SHA:
        raise ValueError("fixed 250921 runtime changed")
    for name, expected in SOURCE_HASHES.items():
        if digest(SOURCE / name) != expected:
            raise ValueError(f"reviewed 3013 source changed: {name}")
    report = json.loads((SOURCE / REPORT).read_text(encoding="utf-8"))
    windows = report["inputs"]["disassembly"]
    if len(windows) != len(WINDOWS):
        raise ValueError("3013 consumer/helper window inventory changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    checked = []
    observed = {}
    for window, (start, end) in zip(windows, WINDOWS):
        if window["binary_sha256"] != RUNTIME_SHA or window["status"] != "collected":
            raise ValueError("3013 machine report provenance changed")
        position = start
        for ins in window["instructions"]:
            rva = int(ins["rva"], 16)
            raw = bytes.fromhex(ins["bytes"])
            if rva != position or image.get_data(rva, len(raw)) != raw:
                raise ValueError(f"3013 machine bytes changed at {rva:#x}")
            decoded = list(decoder.disasm(raw, image.OPTIONAL_HEADER.ImageBase + rva, count=1))
            if len(decoded) != 1 or f"{decoded[0].mnemonic} {decoded[0].op_str}" != ins["instruction"]:
                raise ValueError(f"3013 decoded instruction changed at {rva:#x}")
            if rva in SITES:
                if ins["instruction"] != SITES[rva]:
                    raise ValueError(f"3013 branch site changed at {rva:#x}")
                observed[f"0x{rva:x}"] = {"instruction": ins["instruction"], "bytes": ins["bytes"]}
            position += len(raw)
        if position != end:
            raise ValueError(f"3013 machine window incomplete at {start:#x}")
        checked.append({"first_rva": hex(start), "end_rva_exclusive": hex(end),
                        "instructions": len(window["instructions"]),
                        "sha256": hashlib.sha256(image.get_data(start, end-start)).hexdigest()})
    if len(observed) != len(SITES):
        raise ValueError("3013 branch site list incomplete")
    dispatch = list(decoder.disasm(image.get_data(0x461CB, 19),
                                   image.OPTIONAL_HEADER.ImageBase + 0x461CB))
    if [(ins.mnemonic, ins.op_str) for ins in dispatch[:4]] != [
            ("mov", "r8, rbx"), ("mov", "rdx, r14"),
            ("mov", "rcx, r13"), ("call", "0x14003f620")]:
        raise ValueError("3013 shared dispatcher changed")
    if (len(dispatch) < 5 or dispatch[4].mnemonic != "jmp"
            or dispatch[4].op_str != "0x1400463ce"):
        raise ValueError("3013 shared dispatch continuation changed")
    probes = {}
    for probe in report["inputs"]["probes"]:
        rva = int(probe["request"]["rva"], 16)
        raw = bytes.fromhex(probe["hex"])
        if probe["binary_sha256"] != RUNTIME_SHA or image.get_data(rva, len(raw)) != raw:
            raise ValueError(f"3013 table or limit values changed at {rva:#x}")
        probes[hex(rva)] = probe["hex"]
    items = {row["id"]: row for row in read_rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in read_rows(ROOT / "docs/maniac-250921-command-inventory.csv", ",")
                     if row["opcode"] == "3013")
    destinations = set(BRANCH_TO_ACCEPTANCE.values())
    if (set(OPERATION_TO_ACCEPTANCE) != set(range(23))
            or set(inventory["effective_branch_ids"].split(";")) != set(BRANCH_TO_ACCEPTANCE)
            or set(inventory["acceptance_ids"].split(";")) != destinations
            or items["W03-003"]["item_kind"] != "summary"):
        raise ValueError("3013 branch inventory and acceptance hierarchy disagree")
    for key in destinations:
        if (items[key]["item_kind"] != "leaf" or items[key]["parent_id"] != "W03-003"
                or items[key]["result_type"] != "feature" or key not in inventory["remaining_questions"]):
            raise ValueError(f"3013 branch lacks functional destination: {key}")
    submodes = [row for row in read_rows(ROOT / "docs/maniac-submode-ledger.csv", ",")
                if row["opcode"] == "3013"]
    if {row["ID"] for row in submodes} != {
            "3013:command", *(f"3013:0:{op}" for op in range(23))}:
        raise ValueError("3013 candidate row identities changed")
    for row in submodes:
        if row["ID"] == "3013:command":
            if set(row["acceptance_ids"].split(";")) != destinations:
                raise ValueError("3013 candidate summary lacks acceptance links")
        elif row["acceptance_ids"] != OPERATION_TO_ACCEPTANCE[int(row["ID"].rsplit(":", 1)[-1])]:
            raise ValueError(f"3013 operation candidate lacks destination: {row['ID']}")
    audit = {
        "original_runtime_sha256": RUNTIME_SHA,
        "reviewed_source_sha256": SOURCE_HASHES,
        "consumer_and_helper_windows": checked,
        "shared_dispatch_call": {hex(ins.address-image.OPTIONAL_HEADER.ImageBase): ins.bytes.hex()
                                 for ins in dispatch[:5]},
        "dispatch_and_consumer_instructions": observed,
        "fixed_table_and_limit_bytes": probes,
        "effective_branch_to_acceptance": BRANCH_TO_ACCEPTANCE,
        "operation_to_acceptance": {str(op): OPERATION_TO_ACCEPTANCE[op] for op in range(23)},
        "parameter_rule": "A0 operation 0..22; A1 low nibbles independently select signed A2 base, A3 count and A4 operand modes. Base addressing differs from ordinary numeric lookup: mode 1 forces a negative reference's result into the current self bank; 2..15 use ca4f0 and preserve self polarity through both reads. Count and numeric operand use literal/one-read/two-read. Missing fields read zero; extra Args, unused high flag bits and Text are retained but have no payload role.",
        "guard_and_storage_rule": "A2 base and A3 count resolve before A0 dispatch. Base zero, signed count<=0 or A0 outside 0..22 return. Array helpers grow global or current self banks, including source arrays and an extra slot. Binary operations require resolved A4!=0; copy 0/22 and swap 1 prepare pointers before this check, other binary operations check first. Extreme signed ranges and copy byte counts retain original overflow/error behavior and require explicit safe-deviation verification.",
        "operation_rule": "0/22 are directional memmove copies; 1 swaps forward in place; 2/3 signed single sort have an unsafe negative-self base defect; 4/19 use the shared LCG with final singleton draw; 5 enumerates with uint32 wrap; 6..15 arithmetic/bitwise/shift via 37bd0, with divide/remainder zero divisor replaced by 1, signed clamp for 6..10 and raw32 for 11..15; 16 dereferences with A2-sign-dependent reference polarity; 17/18 paired sort keys then values; 20 fills; 21 reverses. Alias order is operation specific.",
        "context_and_refresh_rule": "Shared dispatcher reaches map autostart/parallel, called common and battle event lists; a 3019-generated 3013 is also a candidate route. Operations 0..15,17..20,22 can invoke per-map-event 61b20 refresh when driver byte 26944a is zero; 16 and 21 skip this explicit refresh; guarded no-ops do not add refresh. Runtime behavior and save/restart acceptance remain separate.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event",
                                "battle event page", "3019-generated candidate"],
        "remaining_questions": [
            "W03-010: all mode/short-record/guard/array-preparation and signed boundary parity.",
            "W03-011: copy/swap/reverse overlap, alias and zero-A4 preparation order.",
            "W03-012: signed single/paired sort, duplicate keys, alias and negative-self safe deviation.",
            "W03-013: shared LCG seed/start/restore and single/paired shuffle swap order.",
            "W03-014: enumerate/fill numeric A4 modes and uint32 wrap.",
            "W03-015: all arithmetic/bitwise/shift operators, clamp and zero-divisor edges.",
            "W03-016: dereference source-bank polarity, alias and INT_MIN.",
            "W03-017: driver-dependent map event refresh, all applicable contexts and save/restart effects.",
            "241028 Windows, 250921 Web and Android are separately scoped.",
        ],
        "acceptance_ids": sorted(destinations),
        "scope": "250921 Windows opcode 3013 branch inventory only. Fixed original bytes and reviewed contract do not establish Player feature parity; old Player-only tests are partial evidence.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(checked)} machine windows, 23 operation destinations")


if __name__ == "__main__":
    main()
