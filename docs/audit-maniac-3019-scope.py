"""Bind the 250921 Call Command branch inventory to the fixed original runtime."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3019-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
REPORT = "analysis-work/reports/03019-58acb09a5d9e03ce662b52f4b646f86ceb4ec18942b286092f3d1230e77574ce.json"
HELPER_REPORT = "analysis-work/reports/03019-f29c3bb51027535f33e26dca13d615f1418970961686d0c38cb9685d93b6183d.json"
SOURCE_HASHES = {
    "commands/03019.md": "70884fa7432d4e0a6800f26e3bd5b8153f835f4141a0bf511e595bd0741c7f83",
    "evidence/CallCommand-250921.md": "13165a1096c4eee3c3caac4196471616b079110dd2c12ae4b2ad49552bc0af1e",
    "evidence/runtime-250921/00045660.c": "444c3f28068a970f57be85734d5f95e5718c659cecba9c04db30220b2d8c382b",
    REPORT: "8cdb77c756c5d7e2a370437d71c8f84e14b27808d28c945d23296fcd3d7c7a4c",
    HELPER_REPORT: "83e0faad6982f42aa5fc5898cb56d5e9936993a7a82f0082ab4a3c030d865c23",
}
FIRST_RVA = 0x459BD
END_RVA = 0x46177  # exclusive: subsequent report bytes belong to other opcodes
SITES = {
    0x459BD: "mov eax, dword ptr [rbx + 4]",          # copied indent
    0x459C6: "mov edx, dword ptr [rbx + 0x20]",       # original argument count
    0x459EC: "and ecx, 0xf",                          # opcode reference nibble
    0x45ACE: "cmp eax, 0xbcb",                        # generated 3019 guard
    0x45AD3: "je 0x1400463db",
    0x45AE5: "shr edx, 4",                            # Text reference nibble
    0x45BE3: "movzx edx, r12b",                       # argument source nibble
    0x45BF2: "sub edx, 1",
    0x45BFB: "sub edx, 2",
    0x45C04: "cmp edx, 1",
    0x45C9F: "xor edx, edx",                          # expression source
    0x45DC3: "mov edx, dword ptr [rbx + 0x20]",      # inline source
    0x45EB9: "call 0x1400491f0",                     # inline buffer
    0x45FE1: "cmp dword ptr [rbx + 0x20], 2",        # array base source
    0x460CA: "test edi, edi",                         # base zero
    0x460D0: "jle 0x140046112",                      # nonpositive count
    0x460D6: "call 0x14008c800",                     # global/self array
    0x46112: "mov edx, 1",                           # zero-count sentinel
    0x4612A: "mov rax, qword ptr [rip + 0x23423f]", # synthetic Args
    0x46166: "cmp edx, 0x573",                       # shared dispatch range
    0x46172: "jmp 0x1400463db",                      # unknown target
}
BRANCH_TO_ACCEPTANCE = {
    "3019:opcode-literal-single-indirect-and-generated-3019-skip": "W03-005",
    "3019:text-literal-direct-indirect-and-short-records": "W03-005",
    "3019:array-raw-single-indirect-global-self-and-count": "W03-006",
    "3019:array-zero-base-or-nonpositive-count": "W03-006",
    "3019:inline-mode-words-raw-single-indirect-and-count": "W03-007",
    "3019:expression-byte-stream-results-and-side-effects": "W03-008",
    "3019:same-frame-redispatch-valid-unknown-busy-and-indent": "W03-009",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    if digest(RUNTIME) != RUNTIME_SHA:
        raise ValueError("fixed 250921 runtime changed")
    for name, expected in SOURCE_HASHES.items():
        if digest(SOURCE / name) != expected:
            raise ValueError(f"reviewed 3019 source changed: {name}")
    report = json.loads((SOURCE / REPORT).read_text(encoding="utf-8"))
    window = report["inputs"]["disassembly"][0]
    if window["binary_sha256"] != RUNTIME_SHA or window["status"] != "collected":
        raise ValueError("fixed 3019 machine report changed")
    selected = [ins for ins in window["instructions"] if int(ins["rva"], 16) < END_RVA]
    if not selected or int(selected[0]["rva"], 16) != FIRST_RVA:
        raise ValueError("3019 machine window does not begin at dispatch entry")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    observed = {}
    position = FIRST_RVA
    for ins in selected:
        rva = int(ins["rva"], 16)
        raw = bytes.fromhex(ins["bytes"])
        if rva != position or image.get_data(rva, len(raw)) != raw:
            raise ValueError(f"3019 machine window changed at {rva:#x}")
        decoded = list(decoder.disasm(raw, image.OPTIONAL_HEADER.ImageBase + rva, count=1))
        if len(decoded) != 1 or f"{decoded[0].mnemonic} {decoded[0].op_str}" != ins["instruction"]:
            raise ValueError(f"3019 decoded instruction changed at {rva:#x}")
        if rva in SITES:
            if ins["instruction"] != SITES[rva]:
                raise ValueError(f"3019 branch site changed at {rva:#x}")
            observed[f"0x{rva:x}"] = {"instruction": ins["instruction"], "bytes": ins["bytes"]}
        position += len(raw)
    if position != END_RVA or len(observed) != len(SITES):
        raise ValueError("3019 machine window incomplete")
    helper_windows = []
    helper_report = json.loads((SOURCE / HELPER_REPORT).read_text(encoding="utf-8"))
    for helper in helper_report["inputs"]["disassembly"][1:]:
        if helper["binary_sha256"] != RUNTIME_SHA:
            raise ValueError("3019 helper report uses another runtime")
        for ins in helper["instructions"]:
            rva = int(ins["rva"], 16)
            raw = bytes.fromhex(ins["bytes"])
            decoded = list(decoder.disasm(image.get_data(rva, len(raw)),
                                         image.OPTIONAL_HEADER.ImageBase + rva, count=1))
            if (len(decoded) != 1 or decoded[0].bytes != raw
                    or f"{decoded[0].mnemonic} {decoded[0].op_str}" != ins["instruction"]):
                raise ValueError(f"3019 helper changed at {rva:#x}")
        helper_windows.append({"first_rva": helper["instructions"][0]["rva"],
                               "last_instruction_rva": helper["instructions"][-1]["rva"],
                               "instructions": len(helper["instructions"])})
    with (ROOT / "docs/maniac-acceptance-items.tsv").open(encoding="utf-8-sig", newline="") as stream:
        items = {row["id"]: row for row in csv.DictReader(stream, delimiter="\t")}
    with (ROOT / "docs/maniac-250921-command-inventory.csv").open(encoding="utf-8-sig", newline="") as stream:
        inventory = next(row for row in csv.DictReader(stream) if row["opcode"] == "3019")
    if (set(inventory["effective_branch_ids"].split(";")) != set(BRANCH_TO_ACCEPTANCE)
            or set(inventory["acceptance_ids"].split(";")) != set(BRANCH_TO_ACCEPTANCE.values())
            or items["W03-004"]["item_kind"] != "summary"):
        raise ValueError("3019 branch inventory and acceptance hierarchy disagree")
    for key in set(BRANCH_TO_ACCEPTANCE.values()):
        if (items[key]["item_kind"] != "leaf" or items[key]["parent_id"] != "W03-004"
                or items[key]["result_type"] != "feature" or key not in inventory["remaining_questions"]):
            raise ValueError(f"3019 branch lacks a stable functional destination: {key}")
    with (ROOT / "docs/maniac-submode-ledger.csv").open(encoding="utf-8-sig", newline="") as stream:
        submodes = [row for row in csv.DictReader(stream) if row["opcode"] == "3019"]
    if {row["ID"] for row in submodes} != {
            "3019:command", *(f"3019:A0[7:4]:{mode}" for mode in range(5))}:
        raise ValueError("3019 candidate row identities changed")
    if set(submodes[0]["acceptance_ids"].split(";")) != set(BRANCH_TO_ACCEPTANCE.values()):
        raise ValueError("3019 candidate summary lacks functional acceptance links")
    for row in submodes[1:]:
        mode = int(row["ID"].rsplit(":", 1)[-1])
        target = "W03-006" if mode in (0, 1, 2) else "W03-007" if mode == 3 else "W03-008"
        if row["acceptance_ids"] != target:
            raise ValueError(f"3019 argument-source candidate lacks destination: {mode}")
    partial_evidence = {}
    for name in ("generated", "modes"):
        path = ROOT / f"docs/evidence/maniac-250921-common1005-{name}-audit.json"
        previous = json.loads(path.read_text(encoding="utf-8"))
        if previous["original_sha256"] != RUNTIME_SHA or len(previous["cases"]) != 8:
            raise ValueError(f"3019 partial evidence provenance changed: {name}")
        partial_evidence[path.relative_to(ROOT).as_posix()] = digest(path)
    audit = {
        "original_runtime_sha256": RUNTIME_SHA,
        "reviewed_source_sha256": SOURCE_HASHES,
        "consumer_window": {"first_rva": hex(FIRST_RVA), "end_rva_exclusive": hex(END_RVA),
                            "instructions": len(selected),
                            "sha256": hashlib.sha256(image.get_data(FIRST_RVA, END_RVA-FIRST_RVA)).hexdigest()},
        "dispatch_and_consumer_instructions": observed,
        "reviewed_allocation_array_and_indirect_helpers": helper_windows,
        "partial_differential_audit_sha256": partial_evidence,
        "effective_branch_to_acceptance": BRANCH_TO_ACCEPTANCE,
        "parameter_rule": "A0 low nibbles independently select A1 generated opcode, A2 argument source, A3 count and A4 Text modes. Each numeric selector uses literal 0, one lookup 1, or indirect lookup 2..15. Original argument count is retained; absent ordinary fields read zero. Unknown high mode bits and extra arguments are preserved.",
        "argument_rule": "Array source mode 0 uses raw signed A2, mode 1 uses one lookup, modes 2 and 5..15 use indirect lookup; signed negative base enters the current self bank. Array count uses the A3 numeric mode. Base zero or count below one gives zero generated Args but a one-element storage sentinel. Inline mode 3 treats raw A2 as a mode-word index, reads A5 onward, uses one low nibble per group of four generated values and clamps negative count to zero. Mode 4 evaluates the little-endian expression stream from A5 onward until its byte cursor reaches the original Args end; A3 does not bound result count. Trailing zero tokens can emit zero values.",
        "text_rule": "Text mode 0 transfers the original command Text; 1 reads string variable A4; 2..15 resolves numeric A4 then reads a string variable. Text is captured before argument expression evaluation, which may mutate its owner.",
        "dispatch_rule": "Generated opcode 3019 skips Text/Args construction and returns to ordinary fetch. Other generated opcodes retain caller indent, owner/frame and already advanced real cursor, then redispatch in the same interpreter invocation. Unknown opcodes use the normal default route. A busy generated command can rewind the original 3019 cursor, causing regeneration and expression reevaluation on retry; generated target behavior and saved frames remain separate functional acceptance.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event",
                                "battle event page", "all shared-dispatch target opcodes"],
        "context_evidence": "The reviewed block lies inside the shared interpreter dispatcher and has no owner-mode guard. Four original/Player 3019-to-1005 context fixtures establish reachability for one generated target, while other targets and retry semantics remain open.",
        "existing_partial_evidence": ["W02-029: four-context inline literal 3019-to-1005 differential",
                                      "W02-030: four-context global/self/indirect/expression 3019-to-1005 differential"],
        "remaining_questions": [
            "W03-005: opcode and Text selectors, short records, dynamic Text lifetime and 3019 self-skip beyond the fixed 1005 examples.",
            "W03-006: array base/count modes, signed source banks, zero/negative counts, side effects and boundaries.",
            "W03-007: inline mode-word overlap, all value reference modes, signed count and source boundaries.",
            "W03-008: expression stream/result count, evaluation order, side effects, errors and shared W03-002 grammar integration.",
            "W03-009: same-frame target redispatch, unknown target, busy retry/re-evaluation, indent and all applicable caller contexts.",
            "241028 Windows, 250921 Web and Android remain separately scoped.",
        ],
        "acceptance_ids": sorted(set(BRANCH_TO_ACCEPTANCE.values())),
        "scope": "250921 Windows opcode 3019 branch inventory only. Original machine bytes and reviewed contract are fixed; this audit does not validate Player feature parity or every generated target.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(selected)} instructions, {len(BRANCH_TO_ACCEPTANCE)} branch groups")


if __name__ == "__main__":
    main()
