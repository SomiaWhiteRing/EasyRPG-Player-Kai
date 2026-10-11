"""Pin Windows 250921 Selfvar scope without accepting untested Player behavior."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3036-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
CONTRACT_SHA = "958afbb351f9ff917f88dcfd54a70ea879ddb8c04d653d57ae6aeaafd9ef9bf6"
SOURCES = {
    "commands/03036.md": "1f54a14d7b6691a5a1bb2340f47edc6b695688f4bce9e41dbe59b1f16f604de9",
    "evidence/SelfVariables-250921.md": "4257f0cf3fa941172520bed2aa46b7dc9afaec78b61e82acc75b80747545c765",
    "evidence/runtime-250921/0000eed0.c": "75bab992ce9cb8ca07844b8bee4930338837e440600d3ce87bc438fcac7f01ab",
    "evidence/runtime-250921/0000e2b0.c": "400e41208b7560d6815c3471f5c4584285efc2b882979119344ce61e204197fc",
    "evidence/runtime-250921/0000e8a0.c": "1251517c10fb01e7c2ccfc8af14ab70c6dd472b22e278e4f1c059cc9236f6355",
    "evidence/runtime-250921/00045660.c": "444c3f28068a970f57be85734d5f95e5718c659cecba9c04db30220b2d8c382b",
    "evidence/native-250921/00041f60.c": "44b45f5d6ca5202d29a6a5811872696e3d66ac1496d2f2b05156ee83e85d90a2",
    "evidence/managed-extra-250921/cmdcs.SelfvarControl.cs": "1116eee306c0b171765359f1d568ac2ae93850cde123f43b8deca62a17999aec",
    "../Maniacs-250921/evidence/250921/SelfvarControl.txt": "edc1d4d3fc810c0f95ef09b0ee891ac23a09c012b4cdd15363c813aeb1ad0704",
    "analysis-work/reports/03036-b28d2aaa98bea8bb29b59a7801f706bc10b6b8ad7118e915b2f7f7f9a7568a26.json": "82becc847d7087ccbb624f4ea1523e79186ce0a538d7f43cac7940c1c062f664",
}
WINDOWS = (
    (0x46967, 0x46A0F, "01806f645ef9615c912fe3e491f9f66ccdf3a43c73b963306f4eeada985c2f74", True),
    (0xEED0, 0xFE22, "2a1bf8ab2524fb94a59e5661afe7afa34823c2cf65e0dc5038c9e9986945ef93", False),
    (0xE2B0, 0xE897, "29d672a01874bea7811a40ae8437e991b95ac8b752b2be9882d3e7b618a25dcd", False),
    (0xE8A0, 0xEEC2, "742ad61c956ca1da3a14c0519e1b8c93b6ee5753e4453af9259c420f29cec4e2", False),
    (0xE050, 0xE2A0, "828d23e576fa7bfd855ac7d819e02a0aa7745f8d19e96ccc060b48bcba7a1294", False),
    (0xD020, 0xD1B2, "5353505cd2432f5a9c616a852da4551398d76faf82015e0bb13efab855ccf10e", False),
    (0xD1C0, 0xD480, "ba6a6fdc1b2a6de74ac318ce7bd3083ffbb890ef9d33ba133bacdb8b8bae481f", False),
    (0xD490, 0xD610, "96d8767b375b5e8cf2e341209719c0ff50bf6690bbb2a9b2d1c40e2eb9cf8df8", False),
    (0xD620, 0xD7EC, "54dd6720c6e51132b32ce7ff027d8aa6c2521270808a3a132845d9e997b68ffc", False),
    (0xD7F0, 0xD8B5, "214d800c926d48579dce548c8376bf6222a31c6d259981641c7484c016059cf2", False),
    (0xD8C0, 0xDB4E, "21fcdd3f2edec8403ad35e80eed4c1cfe57099f93e22515c8ec0b3c6ad06ae6e", False),
    (0xDB50, 0xDD24, "23c1631fd20f62269a40527521c1541969b08bab7ca508603eaf4ee4d8d56865", False),
    (0xDD30, 0xE045, "55f4cc7e820dcb5868e7976dc092db05607d0ef4a24085ecf097eeae2ef11b11", False),
    (0x17C750, 0x17C849, "6f07d4e3b01241752e231042b73ec54b80c644b9e5854305e2c16987ca87522e", False),
    (0x17C350, 0x17C3B6, "960478f4a81e65129ee2615396269817e3637ac53f43b3a8dc2eb4090db9e593", False),
    (0x17C3C0, 0x17C42A, "27046952fc6d4435146deb7f1a8e23b8dc6ae695ccbb51e2b3635f858cb2cf54", False),
    (0x17D4B0, 0x17D518, "821209013ea6a58a35997e3788fdd21e8d602a168c8cc68333219ba1c55405a7", False),
    (0x17D5A0, 0x17D60A, "a482d818e9785d61460989376d35fd096d1be16851455b258f22b67207cc370c", False),
    (0x17D610, 0x17D678, "32f80d1e45a92e4d7d988f17aafb13be8c0a0f93f94c7e89088dcc19665c11fc", False),
    (0x8C900, 0x8C995, "f0d079d2d170cd0d0da1c089280280889a02a7e0d75082b04de82f598bf55cc4", False),
    (0x8CF30, 0x8CFC5, "496f94ff74f52fe2f089b115ceccfacd0dafe9b50e27f81ce09af50105ef30f7", False),
    (0x4B450, 0x4B56C, "0aa77a9eee1337988d06a7c56ad7679fdc35995d6aa3078c55e4077398d347ba", False),
    (0x4C5E0, 0x4C6B1, "86fa364eabd2c814cd34d3360f3c43ae958eff2c55fcf7dd08f541826225febf", False),
    (0x4BFE0, 0x4C2D6, "16f05690832e248bb012d5a1ee16aae47952638e7c5e78fa2de6c177aa9f23a4", False),
)
ENV_FEATURES = ";".join(f"W01-{index:03d}" for index in range(18, 23))
BRANCH_ACCEPTANCE = {
    "3036:global-and-unknown-environment": "W01-001;W01-002;W01-018",
    "3036:common-explicit-id-and-frame-fallback": "W01-019",
    "3036:map-explicit-id-zero-and-10005-fallback": "W01-020",
    "3036:current-frame": "W01-003;W01-004;W01-021",
    "3036:caller-frame-and-missing-context": "W01-022",
    "3036:set-environment-allocation-before-gates": ENV_FEATURES + ";W01-028",
    "3036:clear-init-existing-only-and-wildcards": ENV_FEATURES,
    "3036:clear-init-map-selector-mode5": "W01-020;W01-021;W01-022",
    "3036:common-name-width-budget-and-text-cursor": "W01-023",
    "3036:shared-command-cache-and-reexecution": "W01-024",
    "3036:common-name-string-indirection-nul-and-lookup": "W01-025",
    "3036:set-scalar-target-indirection-slot0-and-count": ENV_FEATURES,
    "3036:set-scalar-number-and-kind-fallback": ENV_FEATURES,
    "3036:set-scalar-switch-raw-byte-and-global-number-reads": ENV_FEATURES,
    "3036:set-scalar-string-global-number-reads-and-retention": ENV_FEATURES,
    "3036:set-literal-string-shared-text-budget": "W01-023;W01-024",
    "3036:set-array-id-modes-and-current-bank-indirection": "W01-026;W01-027",
    "3036:set-array-number-switch-bulk-overlap": "W01-026",
    "3036:set-array-string-forward-alias-and-null-empty": "W01-027",
    "3036:set-array-source-growth-nonpositive-count-and-size": "W01-028",
    "3036:set-array-same-vector-negative-address-defect": "W01-029",
    "3036:clear-kind-all-range-negative-and-no-growth": ENV_FEATURES + ";W01-029",
    "3036:init-object-count-release-and-global-retained-capacity": ENV_FEATURES,
    "3036:init-sparse-map-wildcard-null-defect": "W01-020;W01-029",
    "3036:short-extra-unknown-record-and-formatter-divergence": "W01-030",
    "3036:dispatch-refresh-config-and-context-continuation": "W01-030",
}
FEATURES = tuple(f"W01-{index:03d}" for index in range(18, 31))
ACCEPTANCE = tuple(f"W01-{index:03d}" for index in range(1, 5)) + FEATURES


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
            raise ValueError(f"reviewed 3036 source changed: {name}")
    catalog = json.loads((SOURCE / "reviewed-contracts.json").read_text(encoding="utf-8"))
    contract = next(row for row in catalog["commands"] if row["opcode"] == 3036)
    if sha(json.dumps(contract, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")) != CONTRACT_SHA:
        raise ValueError("reviewed 3036 contract changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    for lo, hi, expected, code in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        if sha(blob) != expected:
            raise ValueError(f"3036 machine window changed: {lo:#x}")
        if code and sum(insn.size for insn in decoder.disasm(
                blob, image.OPTIONAL_HEADER.ImageBase + lo)) != len(blob):
            raise ValueError("3036 dispatch is not complete code")
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": expected, "decoded_as_code": code})
    normalized = (3036 & 0x3FF) - (((3036 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x51E, 0x47EC8, 37, 0x47DE8, 0x46967):
        raise ValueError("3036 opcode route changed")
    for rva, expected in ((0x46967, "cmp dword ptr [rbx + 0x20], 9"),
                          (0x4699B, "call 0x14000e8a0"),
                          (0x469B3, "call 0x14000e2b0"),
                          (0x469CB, "call 0x14000eed0"),
                          (0x469FB, "call 0x140061b20"),
                          (0x46A08, "mov al, 1"), (0x46A0A, "jmp 0x140046ef3")):
        instruction = next(decoder.disasm(image.get_data(rva, 15),
                                          image.OPTIONAL_HEADER.ImageBase + rva))
        if f"{instruction.mnemonic} {instruction.op_str}" != expected:
            raise ValueError(f"3036 key instruction changed: {rva:#x}")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3036")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3036"]
    if (items["C250921-03036"]["status"] != "closed"
            or any(items[key]["item_kind"] != "summary" for key in ("W01-006", "W01-008", "W01-009"))
            or any(items[key]["status"] != "closed" for key in ACCEPTANCE[:4])
            or any(items[key]["status"] != "open" or items[key]["result_type"] != "feature" for key in FEATURES)
            or any(items[f"W01-{i:03d}"]["parent_id"] != parent
                   for parent, indices in (("W01-006", (*range(18, 23), 30)),
                                           ("W01-008", range(23, 26)),
                                           ("W01-009", range(26, 30))) for i in indices)
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(ACCEPTANCE)
            or inventory["inventory_status"] != "implemented_partial_unverified"
            or {row["ID"] for row in candidates} != {"3036:command"} | set(BRANCH_ACCEPTANCE) | set(ACCEPTANCE[:4])
            or any(row["acceptance_ids"] != (";".join(ACCEPTANCE) if row["ID"] == "3036:command"
                   else row["ID"] if row["ID"] in ACCEPTANCE[:4] else BRANCH_ACCEPTANCE[row["ID"]]) for row in candidates)):
        raise ValueError("3036 branch and acceptance mapping incomplete")
    player = (ROOT / "src/game_interpreter.cpp").read_text(encoding="utf-8")
    if "case Cmd::Maniac_Selfvar:\n\t\t\treturn CommandManiacSelfvar(com);" not in player:
        raise ValueError("Player 3036 dispatch inventory is stale")
    result = {
        "runtime_sha256": RUNTIME_SHA, "source_sha256": SOURCES,
        "reviewed_contract_sha256": CONTRACT_SHA, "machine_windows": windows,
        "opcode_mapping": {"opcode": 3036, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}", "branch_rva": f"0x{target:x}",
                           "operation_helpers": {"0": "0xeed0", "1": "0xe2b0", "2": "0xe8a0"}},
        "effective_branch_ids": list(BRANCH_ACCEPTANCE), "branch_acceptance_ids": BRANCH_ACCEPTANCE,
        "acceptance_ids": list(ACCEPTANCE), "preserved_partial_acceptance_ids": list(ACCEPTANCE[:4]),
        "environment_rule": "0/unknown global;1 common ID/name;2 map/event ID;3 current;4 caller. Set resolves target then source and allocates before gates. Common0 and map/event0/10005 use frame/backward stack fallback. Clear/Init use existing banks only, negative common/map/event wildcards; mode5 map selector one-read forced-negative differs from Set. Missing current/caller Set can fail non-returning; Clear/Init skip.",
        "scalar_rule": "Target modes1 literal/2 current numeric read with initial-negative propagation, abs32 selected-bank slot including0; other skip. Number kind0/other raw/direct/indirect. Self switch modes1 switch/2 numeric then switch; global uses numeric reads; broadcast low8. Self string direct/indirect; global one/two numeric reads before string lookup. Other string modes consume shared Text cursor; retain scalar before destination growth. uint32 count growth plus size guard can occur without writes.",
        "text_rule": "Writer cuts trailing zero fields (default nine values), Text concatenates target/source literal names and scalar string without delimiter; A1/A4/A6 are ANSI byte budgets. Runtime width per UTF16 unit1 for U+0000..00FF/FF61..FF9F, else2, advances accepted units. Managed reader advances requested bytes; formatter advances actual clamped bytes. Both literal name selectors share command+8 cache, cache hits skip Text consumption, including reexecution. Names exact case/full-length lookup; string-variable names truncate at NUL. Cache not serialized.",
        "array_rule": "Target/source modes1/2 only, mode2 current numeric banks with negative propagation. Different vectors abs IDs and source growth; same-vector capacity abs(signed max) but pointers signed. Numeric/switch bulk move overlap; string forward reference-counted copy can propagate writes and distinguishes null/empty. Nonpositive count may allocate; uint32 growth and signed byte counts require safe defect policy.",
        "clear_init_rule": "Clear resolves target once without Set sign adjustment, does not grow; signed start/available range, negative count remainder, kind3 number/string/switch, unknown none. Init ignores source/slot/kind/array, count covers environment objects; self vectors freed, objects kept; globals logical ends cleared capacity retained. Sparse map all-events Init can dereference null.",
        "dispatch_record_rule": "Missing fields0, A9 whole0/1/2 only; unknown helper skipped but map refresh still occurs when26944a=0. Direct success, no opcode-owned wait. Preserve ignored flags/Args/Text/tails; formatter unknown op/array/kind labels are not runtime behavior.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "remaining_questions": [
            "W01-018..022: five environment scalar/Clear/Init, identity/allocation/fallback/wildcard parity beyond accepted fixed samples.",
            "W01-023..025: name/literal budgets, cache/reexecution/shared cursor, string lookup/NUL and missing names.",
            "W01-026..029: bulk/string overlap, source/target growth, count/size, original negative-pointer/null-child defects and documented safe deviations.",
            "W01-030: short/extra/unknown preserved records, configuration map refresh, four applicable contexts and saved continuation.",
            "W01-007 shared consumers, 241028, Web/Android remain separate; no general bank-consumer closure.",
        ],
        "scope": "250921 Windows static 3036 scope only; Player dispatcher exists with W01-001..004 partial fixed differential evidence. Remaining feature leaves open; original unsafe paths require documented safe deviations per completion plan.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(BRANCH_ACCEPTANCE)} Selfvar branches; four accepted samples retained, thirteen feature leaves open")


if __name__ == "__main__":
    main()
