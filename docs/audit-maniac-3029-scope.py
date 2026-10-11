"""Pin Windows 250921 Control Message scope and unresolved feature leaves."""

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
OUTPUT = ROOT / "docs/evidence/maniac-250921-3029-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
CONTRACT_SHA = "0cae2f99cf262c0e04541c8e04b72ea786ebe7202ecbbcfd0a71e8d5a65cb2c3"
SOURCES = {
    "commands/03029.md": "b0f260206b97d3877cf8c8ada5fa715c85e31adb22639331e8b8a29b29816935",
    "evidence/MessageHook-250921.md": "958518bc9973103dc213533696a7d0cbeb3bcc0a83151c268f890fe3c4965502",
    "evidence/runtime-250921/0002f1c0.c": "4bc2281e8e5652313c637d1cc2a7c072d026c7cd408b36e6326b092612a2d09d",
    "evidence/native-250921/00018fa0.c": "9547577611fff332dd0c3fa3f002fef33fc8cf9790e2687bcf7389911d98174b",
    "../Maniacs-250921/evidence/250921/MessageHookControl.txt": "260d82714356a447f1cffbe11e5b2759a7690884184b73a92b12fca30c71b065",
    "analysis-work/reports/03029-e007a06b1312685157b91e791490e05e7fe15d831502c7a7f026deaa01a31e44.json": "d12a3760d937623029def03d0ccc7eb9825aac729517cb1e770316d2d6c413ea",
}
WINDOWS = (
    (0x46925, 0x46934, "14ae86d439ebd4e3bc095a5a632c102e57da603b28c81094d53230703f4dfc57", True),
    (0x2F1C0, 0x2F510, "b5df7908c4c17ec0160b70f55a1953ec69c460445d6e0d56d525a2a127433e49", False),
    (0x1BB220, 0x1BB3F8, "6ba4e03fa092f6a4be457c9f9cd9413b2122492eb98708cb10b40f1a3b879116", False),
    (0x1BBDA0, 0x1BBFA4, "c38e77df1c93ddd9a0eaab9aed577ec83c92aebaec32bc454b74bea6d4670a47", False),
    (0x1BC180, 0x1BC554, "eccbffb2079810ce903c1d718088b7525ead3fcaf2e80b77f7e372a2928c1d5b", False),
    (0x1BBFB0, 0x1BC17B, "6f18ffd656e0020bd2ecaa6e29a0501638091f5de26170527e4db82c539f3ecf", False),
    (0x19A850, 0x19A8BD, "678de9550db6772a0a952659c52f8b3d3eb87618c0c9636b92273d3403915e3a", False),
)
BRANCH_ACCEPTANCE = {
    "3029:selector-literal-id": "W06-019",
    "3029:selector-numeric-variable": "W06-019",
    "3029:selector-indirect-numeric": "W06-019",
    "3029:selector-literal-name": "W06-019",
    "3029:selector-string-name": "W06-019",
    "3029:selector-indirect-string-name": "W06-019",
    "3029:selector-other-zero-and-cache": "W06-019",
    "3029:signed-system-and-user-bases": "W06-019;W06-024",
    "3029:user-hook-packet-and-readback": "W06-020;W06-024",
    "3029:user-escape-mixed-token-parse": "W06-020",
    "3029:create-hook-packet-and-readback": "W06-021;W06-024",
    "3029:destroy-hook-packet-and-readback": "W06-022;W06-024",
    "3029:blit-hook-packet-and-readback": "W06-023;W06-024",
    "3029:hook-mask-and-invalid-event": "W06-019;W06-024",
    "3029:dedicated-stack-budget-wait-and-restore": "W06-024",
    "3029:short-record-text-and-direct-success": "W06-019;W06-024",
}
FEATURES = tuple(f"W06-{index:03d}" for index in range(19, 25))


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
            raise ValueError(f"reviewed 3029 source changed: {name}")
    catalog = json.loads((SOURCE / "reviewed-contracts.json").read_text(encoding="utf-8"))
    contract = next(row for row in catalog["commands"] if row["opcode"] == 3029)
    if sha(json.dumps(contract, ensure_ascii=False, sort_keys=True,
                      separators=(",", ":")).encode("utf-8")) != CONTRACT_SHA:
        raise ValueError("reviewed 3029 contract changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    windows = []
    for lo, hi, expected, code in WINDOWS:
        blob = image.get_data(lo, hi - lo)
        actual_hash = sha(blob)
        if actual_hash != expected:
            raise ValueError(f"3029 machine window changed: {lo:#x}")
        if code and sum(insn.size for insn in decoder.disasm(
                blob, image.OPTIONAL_HEADER.ImageBase + lo)) != len(blob):
            raise ValueError(f"3029 dispatch is not complete code: {lo:#x}")
        windows.append({"begin_rva": f"0x{lo:x}", "end_rva_exclusive": f"0x{hi:x}",
                        "sha256": actual_hash, "decoded_as_code": code})
    normalized = (3029 & 0x3FF) - (((3029 >> 8) & 0xFF) << 3) + 0x19A
    selector_rva = 0x47DF8 + normalized - 0x44E
    slot = image.get_data(selector_rva, 1)[0]
    target_slot_rva = 0x47D54 + 4 * slot
    target = struct.unpack("<I", image.get_data(target_slot_rva, 4))[0]
    if (normalized, selector_rva, slot, target_slot_rva, target) != (
            0x517, 0x47EC1, 33, 0x47DD8, 0x46925):
        raise ValueError("3029 opcode route changed")
    for rva, expected in ((0x46928, "call 0x14002f1c0"),
                          (0x4692D, "mov al, 1"), (0x4692F, "jmp 0x140046ef3")):
        instruction = next(decoder.disasm(image.get_data(rva, 15),
                                          image.OPTIONAL_HEADER.ImageBase + rva))
        actual = f"{instruction.mnemonic} {instruction.op_str}"
        if actual != expected:
            raise ValueError(f"3029 key instruction changed: {rva:#x}: {actual}")
    items = {row["id"]: row for row in rows(ROOT / "docs/maniac-acceptance-items.tsv", "\t")}
    inventory = next(row for row in rows(ROOT / "docs/maniac-250921-command-inventory.csv")
                     if row["opcode"] == "3029")
    candidates = [row for row in rows(ROOT / "docs/maniac-submode-ledger.csv")
                  if row["opcode"] == "3029"]
    if (items["C250921-03029"]["status"] != "closed"
            or items["W06-002"]["item_kind"] != "summary"
            or items["W06-025"]["item_kind"] != "unexpanded"
            or items["W06-025"]["parent_id"] != "W06-002"
            or any(items[key]["parent_id"] != "W06-002" or items[key]["result_type"] != "feature"
                   or items[key]["status"] != "open" for key in FEATURES)
            or inventory["effective_branch_ids"] != ";".join(BRANCH_ACCEPTANCE)
            or inventory["acceptance_ids"] != ";".join(FEATURES)
            or {row["ID"] for row in candidates} != {"3029:command"} | set(BRANCH_ACCEPTANCE)
            or any(row["acceptance_ids"] != (";".join(FEATURES) if row["ID"] == "3029:command"
                                                  else BRANCH_ACCEPTANCE[row["ID"]])
                   for row in candidates)):
        raise ValueError("3029 branch and acceptance mapping incomplete")
    result = {
        "runtime_sha256": RUNTIME_SHA, "source_sha256": SOURCES,
        "reviewed_contract_sha256": CONTRACT_SHA, "machine_windows": windows,
        "opcode_mapping": {"opcode": 3029, "normalized_selector": normalized,
                           "selector_byte_rva": f"0x{selector_rva:x}", "slot": slot,
                           "target_slot_rva": f"0x{target_slot_rva:x}",
                           "branch_rva": f"0x{target:x}", "consumer_rva": "0x2f1c0"},
        "effective_branch_ids": list(BRANCH_ACCEPTANCE),
        "branch_acceptance_ids": BRANCH_ACCEPTANCE, "acceptance_ids": list(FEATURES),
        "selector_rule": "A0 low nibble 0 literal common ID,1 numeric reference,2 indirect numeric,3 literal UTF16 Text name,4 string-variable name,5 indirect string-variable name; others resolve ID0. Literal-name lookup caches only an opaque runtime field. Writer emits seven untrimmed words and mode3 ANSI byte count in A2, but runtime scans whole UTF16 name and does not use A2 as its length. Native formatter compares whole A0; preserve high bits.",
        "destination_rule": "A1 low-byte mask bits1/2/4/8 enable user/create/destroy/blit only when resolved event ID>0. A3/A4 system numeric/text and A5/A6 user numeric/text bases are independent; zero disables each bank, positive global, negative caller self with increasing absolute index. Numeric/string contiguous copy and signed readback use field-specific helpers and raw32 arithmetic.",
        "packet_rule": "User 8 numeric slots, Create 9, Destroy 8, Blit 9; create exports current face name and blit current glyph token to system string base. User E escape parses mixed numeric/text tokens to separate cursors even when hook disabled; final parsed value can write before delimiter helper reports false. Each hook reads only its specified offsets: user window/face XY; create window XY, W/H clamp1..2000, face XY after updated window fallback and boolean transition; destroy immediate/normal via offset7; blit glyph XY clamp0..W/H and two raw colors.",
        "schedule_rule": "Hook helper builds a dedicated common-event interpreter stack, installs it as current only during dispatcher loop under current command budget and wait gates, then restores caller stack before readback. Hook may yield/stop unfinished; out-of-range positive event ID enters checked error. Direct 3029 dispatch returns success without opcode-owned wait; context, reentry and saved continuation need dynamic comparison.",
        "record_rule": "Missing A0..A6 words zero; preserve short/extra Args, high flags and raw Text. Runtime Text is a common-event identifier only for mode3, not translatable message body. Mode3 coordinated renames must maintain writer byte count. All other modes preserve Text/A2 untouched.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page"],
        "remaining_questions": [
            "W06-019: selector/name cache, signed bases, mask, malformed and short/extra record handling.",
            "W06-020: user E mixed token parsing, separate banks, final-token side effects, packet/readback.",
            "W06-021: create packet/name, readback order, dimension clamp and transition.",
            "W06-022: destroy packet and immediate/normal closure.",
            "W06-023: blit token, coordinate clamp and color readback.",
            "W06-024: dedicated stack, wait/budget/reentry/invalid event, four contexts and save/restart.",
            "W06-025 preserves non-3029 message/window/font/choice/control-code scope; C241028-03029 and Web/Android are separate.",
        ],
        "scope": "250921 Windows static 3029 command scope only; Player has no dispatcher and no feature parity accepted.",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(BRANCH_ACCEPTANCE)} 3029 branches mapped to six open W06 leaves")


if __name__ == "__main__":
    main()
