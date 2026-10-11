"""Audit direct references to the 250921 JavaScript control flags.

Only functions in the PE unwind table are scanned. The resulting xrefs narrow
the next control-exception investigation; they do not identify the source of
each C++ exception or prove a complete lifecycle.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_MEM, X86_REG_RIP
import pefile


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-control-flag-xrefs.json"
EXPECTED_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
FLAGS = {0x278FB9, 0x278FBA, 0x278FBB}
SET_SITES = {
    0x1FC59E, 0x1FC5CC, 0x1FC5FA,
    0x1FC684, 0x1FC6AF, 0x1FC6DA,
    0x1FC7A8, 0x1FC7D3, 0x1FC7FE,
    0x1FC8A6, 0x1FC8D4, 0x1FC902,
}
OVERLAPPING_WORD_CLEAR_SITES = {0x5A4D4, 0x1816C6}
CLEAR_SITES = {0x5A4DD, 0x5A4E6, 0x1816CD, 0x1816D4,
               0x18222D, 0x18224D, 0x182270}
CHECK_SITES = {0x18217B, 0x182184, 0x18218D,
               0x1821C5, 0x1821CE, 0x1821DB}


def main() -> None:
    digest = hashlib.sha256(RUNTIME.read_bytes()).hexdigest()
    if digest != EXPECTED_SHA256:
        raise ValueError("250921 English runtime changed")
    image = pefile.PE(str(RUNTIME))
    image.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
    base = image.OPTIONAL_HEADER.ImageBase
    if base != 0x140000000:
        raise ValueError("unexpected runtime image base")
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    xrefs = []
    for function in image.DIRECTORY_ENTRY_EXCEPTION:
        begin = function.struct.BeginAddress
        end = function.struct.EndAddress
        if end <= begin or end - begin > 100000:
            raise ValueError(f"invalid unwind function span: {begin:#x}..{end:#x}")
        for instruction in decoder.disasm(image.get_data(begin, end - begin), base + begin):
            for operand in instruction.operands:
                if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
                    continue
                target = instruction.address + instruction.size + operand.mem.disp - base
                if target not in FLAGS:
                    continue
                site = instruction.address - base
                category = ("set" if site in SET_SITES else "clear" if site in CLEAR_SITES
                            else "check" if site in CHECK_SITES else "unexpected")
                xrefs.append({"rva": f"0x{site:x}", "function_rva": f"0x{begin:x}",
                              "flag_rva": f"0x{target:x}", "instruction":
                              f"{instruction.mnemonic} {instruction.op_str}", "category": category})
    sites = {int(entry["rva"], 16) for entry in xrefs}
    expected = SET_SITES | CLEAR_SITES | CHECK_SITES
    if sites != expected or len(xrefs) != len(expected):
        raise ValueError(f"control flag xrefs changed: missing={expected - sites}, extra={sites - expected}")
    if {entry["function_rva"] for entry in xrefs if entry["category"] == "set"} != {
            "0x1fc570", "0x1fc65c", "0x1fc780", "0x1fc878"}:
        raise ValueError("control flag setter function group changed")
    overlapping = []
    for site in sorted(OVERLAPPING_WORD_CLEAR_SITES):
        instruction = next(decoder.disasm(image.get_data(site, 16), base + site, count=1))
        memory = instruction.operands[0]
        if (instruction.mnemonic != "mov" or memory.type != X86_OP_MEM or
                memory.mem.base != X86_REG_RIP or memory.size != 2 or
                instruction.address + instruction.size + memory.mem.disp - base != 0x278FB8):
            raise ValueError(f"overlapping word clear changed: {site:#x}")
        overlapping.append({"rva": f"0x{site:x}", "instruction":
                            f"{instruction.mnemonic} {instruction.op_str}",
                            "covered_flag_rva": "0x278fb9"})
    audit = {
        "runtime_sha256": digest,
        "unwind_registered_function_count": len(image.DIRECTORY_ENTRY_EXCEPTION),
        "flag_rvas": [f"0x{flag:x}" for flag in sorted(FLAGS)],
        "direct_references": sorted(xrefs, key=lambda entry: int(entry["rva"], 16)),
        "overlapping_word_clear_references": overlapping,
        "scope": "Direct RIP references in 250921 unwind-registered functions plus two overlapping word clears of flag 0x278fb9. Four grouped functions set all three control flags; initializer/teardown and 3030 evaluator clear or inspect them. Leaf functions outside the unwind table, exception throw origins, catch linkage and runtime lifecycle are not established.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(xrefs)} control-flag references")


if __name__ == "__main__":
    main()
