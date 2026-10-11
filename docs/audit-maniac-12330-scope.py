"""Pin Windows 250921 CallEvent branches without accepting untested behavior."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-12330-scope-audit.json"
REFERENCE_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
CONTRACT_SHA = "83f527ef0877a46718e640c8c44c193d0b56e159d2a411b6c3c814c41f5f7163"
SOURCES = {
    "commands/12330.md": "1f97cd7756056a9964d865d460daf87c01f2f244317507183f2ae31e9f5345a3",
    "evidence/CallEvent-250921.md": "224089f7614d65fc02e62fed0ec585cec00cb5f25e94ad24c20d8a7bc63826d5",
    "evidence/runtime-250921/000252f0.c": "485047ec0c04f4f8780785efa6714e257da1a8737a97b78083cf433c2cbb0b9d",
    "evidence/native-250921/000305f0.c": "0fe2c10b1d7218d29fbc2075408234fe934ec26df2d89769957336350882a7a7",
    "evidence/runtime-250921/00030e70.c": "7c649676a9e047a493cfaf9dff38317faaeab5cf01a9f3c53d23df9b374801c6",
    "evidence/NativeBatch01-250921.md": "8724a645a1cd46b056a276915617b9810f4746e49fc7ea346831bcc04b18be94",
    "analysis-work/reports/12330-fd90febfb1340fc18a2e3f3696bfa8b61fc2efff62a9b11f61f40d848dd41818.json": "e99ce18b1b982ecf455d08fa2617f8a5d8a483fb886c8ba0aae10678508e5f62",
}
WINDOWS = {
    0x47A0A: (21, "394a308a6147d259e565ed47d1be7d26cda5e66c803123c3b21c44185ad68365"),
    0x8C210: (75, "d7981c01de1fe01c5669542ded1a8059e4723e5feed3dd643e4611b1f2641c24"),
    0x17D680: (180, "f99080f85f90a28cfd17ca383baf655f9e223f7a2ed76dc01f28add0a645c086"),
    0x8C9E0: (220, "b1c187ba8758cf9000654e45865944eccd762874a7d064e937d7f70a08f66b2e"),
    0x8CCA0: (234, "1b959feb3aa7bcb302c0fdb600c56ceeb4abde83858671a0c9c0aa6a5b529c54"),
    0x17D740: (119, "1112af3e72071a208c23e67336a643d3a870bebee50dc2735bb3f22f126a03bb"),
    0x8CD90: (154, "d1a2f51d3ad62c4421adcffab3e4207fe2c707f95bd2e5017fff5065841d1b5c"),
    0x8C100: (272, "25f55014dc428201e151553f8d40e59ae7a0e9054a1ac0ba61aecedd1f3932ae"),
}
HANDLER_SHA = "dd6d0462fa80e1c1af19bc4bc31f8e0524d575ea6e5efc9763862ed0278d457e"
BRANCH_ACCEPTANCE = {
    "12330:common-selector-literal-variable-indirect-unknown": "W02-004;W02-040;W02-042",
    "12330:map-selector-literal-current-variable-page": "W02-037",
    "12330:map-clone-definition-and-page-error": "W02-038",
    "12330:common-name-literal-cache-and-text-budget": "W02-040",
    "12330:common-name-string-direct-indirect": "W02-004;W02-040",
    "12330:id-offset-literal-variable-indirect": "W02-004;W02-040",
    "12330:typed-number-string-switch-argument-sources": "W02-001;W02-041",
    "12330:typed-argument-sequential-alias-and-bank": "W02-001;W02-041",
    "12330:short-count-extra-unknown-record": "W02-042",
    "12330:zeroed-frame-depth-and-error-order": "W02-042",
    "12330:deferred-return-destination": "W02-043",
    "12330:four-context-call-wait-save-continuation": "W02-046;W02-047;W02-048;W02-049",
    "12330:3019-produced-call-record": "W03-009",
}
ACCEPTANCE = ("W02-001", "W02-004", "W02-037", "W02-038",
              "W02-040", "W02-041", "W02-042", "W02-043", "W02-046", "W02-047", "W02-048", "W02-049", "W03-009")


def sha(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def read_rows(path: Path, delimiter: str) -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream, delimiter=delimiter))


def main() -> None:
    if sha(RUNTIME.read_bytes()) != REFERENCE_SHA:
        raise ValueError("fixed 250921 runtime changed")
    for name, expected in SOURCES.items():
        if sha((SOURCE / name).read_bytes()) != expected:
            raise ValueError(f"reviewed CallEvent source changed: {name}")
    catalog = json.loads((SOURCE / "reviewed-contracts.json").read_text(encoding="utf-8"))
    contract = next(row for row in catalog["commands"] if row["opcode"] == 12330)
    if sha(json.dumps(contract, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")) != CONTRACT_SHA:
        raise ValueError("reviewed CallEvent contract changed")
    report = json.loads((SOURCE / next(name for name in SOURCES if name.startswith("analysis-work/"))).read_text(encoding="utf-8"))
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    checked = []
    windows_by_rva = {int(window["request"]["rva"], 16): window for window in report["inputs"]["disassembly"]
                      if window["request"].get("image") == "runtime"}
    for rva, (size, expected) in WINDOWS.items():
        blob = image.get_data(rva, size)
        if sha(blob) != expected:
            raise ValueError(f"CallEvent machine window changed: {rva:#x}")
        window = windows_by_rva[rva]
        if window["decoded_bytes"] != size or sum(int(entry["rva"], 16) == rva for entry in window["instructions"]) != 1:
            raise ValueError(f"reviewed window changed: {rva:#x}")
        for entry in window["instructions"]:
            address = int(entry["rva"], 16)
            instruction = next(decoder.disasm(image.get_data(address, 16), image.OPTIONAL_HEADER.ImageBase + address))
            if instruction.bytes.hex() != entry["bytes"] or f"{instruction.mnemonic} {instruction.op_str}" != entry["instruction"]:
                raise ValueError(f"reviewed CallEvent instruction changed: {address:#x}")
        checked.append({"rva": f"0x{rva:x}", "size": size, "sha256": expected,
                        "instructions": len(window["instructions"])})
    handler = image.get_data(0x252F0, 0x2657C - 0x252F0)
    if sha(handler) != HANDLER_SHA:
        raise ValueError("CallEvent handler changed")
    dispatch = windows_by_rva[0x47A0A]["instructions"]
    if [entry["instruction"] for entry in dispatch][-3:] != [
            "call 0x1400252f0", "mov al, 1", "jmp 0x140046ef3"]:
        raise ValueError("CallEvent dispatch target changed")
    items = {row["id"]: row for row in read_rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in read_rows(ROOT / "docs/maniac-250921-command-inventory.csv", ",")
                     if row["opcode"] == "12330")
    candidates = [row for row in read_rows(ROOT / "docs/maniac-submode-ledger.csv", ",")
                  if row["opcode"] == "12330"]
    if (items["C250921-12330"]["status"] != "closed"
            or items["W02-001"]["status"] != "closed" or items["W02-004"]["status"] != "closed"
            or any(items[key]["status"] == "closed" for key in ACCEPTANCE[2:] if key != "W02-046")
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(ACCEPTANCE)
            or {row["ID"] for row in candidates if row["ID"].startswith("12330:") and row["ID"] in BRANCH_ACCEPTANCE} != set(BRANCH_ACCEPTANCE)
            or any(row["acceptance_ids"] != BRANCH_ACCEPTANCE[row["ID"]]
                   for row in candidates if row["ID"] in BRANCH_ACCEPTANCE)):
        raise ValueError("CallEvent branch/acceptance mapping incomplete")
    report_out = {
        "reference_sha256": REFERENCE_SHA, "reviewed_source_sha256": SOURCES,
        "reviewed_contract_sha256": CONTRACT_SHA,
        "handler": {"rva": "0x252f0", "end_rva_exclusive": "0x2657c", "sha256": HANDLER_SHA},
        "dispatch_and_helpers": checked, "effective_branch_ids": list(BRANCH_ACCEPTANCE),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE, "acceptance_ids": list(ACCEPTANCE),
        "preserved_partial_acceptance_ids": ["W02-001", "W02-004"],
        "selector_rule": "A0 0/unknown literal common; 1 map literal with 10005 current-event fallback; 2 map numeric event/page; 3 common numeric; 4 double numeric; 5 literal name; 6 string-variable name; 7 numeric-selected string-variable name. A3 low nibble selects A4 literal/one-read/two-read offset, added as int32 only to selected ID.",
        "name_text_rule": "Runtime Text cursor uses UTF16-unit scan with approximate one/two-byte budget, not editor ANSI slicing. Literal selector 5 caches pre-offset ID at command+8 but rescans name on cache hit; literal type-4 arguments continue at the same cursor. Name lookup is case-sensitive and first duplicate wins; string-variable names stop at NUL.",
        "argument_rule": "A6 positive signed count loops even if argument pairs are missing; absent fields resolve zero. Types 0/1/unknown raw number, 2/3 variable number, 4 literal Text string, 5/6 string variable, 7 raw low switch byte, 8/9 stored switch. Number/string/switch slots each start at 1 in persistent callee bank; source read and destination write alternate, so recursive aliasing changes later reads.",
        "frame_and_return_rule": "Old depth >8192 fails. A zeroed 0x40 frame is allocated before event selection, typed arguments assigned before append; common kind 0x25 and map kind 0x15. Map frame stores requested ID separately from resolved definition/self identity. Records with >=6 fields save A3 receiver nibble and raw A5 for later 12310; delivery is not performed by 12330 or ordinary frame end.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page", "3019-produced record"],
        "remaining_questions": [
            "W02-037/038: map selectors, current event/page, clone requested-vs-definition identity, missing map event/page errors.",
            "W02-040: name lookup/cache and shared Text cursor across literal argument strings, offsets and lookup errors.",
            "W02-041/026: all typed argument sources, independent slots, recursion aliasing and persistent banks.",
            "W02-042: short/extra/unknown records, depth, frame/error order, retained text and safe malformed-input policy.",
            "W02-043/007: deferred receiver fields and 12310 return-time delivery/cleanup interaction.",
            "W02-044 children W02-046..049 and W02-010: four-context wait, save/restart and caller continuation.",
            "W03-009: 3019-generated 12330 owner/indent/cursor and same-frame redispatch; target payload remains under W02-037..044.",
            "W11-016: original/Player raw switch-byte-3 LSD discrepancy is separate from W02-041 runtime argument parity.",
            "241028 and Web/Android retain separate version/platform acceptance.",
        ],
        "scope": "250921 Windows original consumer and helper scope only. W02-001/004 are limited prior differential samples; open features and full Player parity are not inferred from static evidence.",
    }
    OUTPUT.write_text(json.dumps(report_out, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(BRANCH_ACCEPTANCE)} CallEvent branches and {len(ACCEPTANCE)} acceptance destinations")


if __name__ == "__main__":
    main()
