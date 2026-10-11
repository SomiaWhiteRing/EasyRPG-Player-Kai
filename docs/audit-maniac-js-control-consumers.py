"""Audit each 250921 evaluator control-flag clear/release/throw path.

This is binary control-flow evidence, not an injected-flag runtime experiment.
The producer origins and game-loop catch/continuation remain separate leaves.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
import pefile

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-control-consumer-audit.json"
EXPECTED_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
FLAGS = (
    (0x278FB9, 0x18217B, 0x182182, 0x1821C5, 0x1821CC, 0x18224D, 0x18225F, 0x18226A, 0),
    (0x278FBA, 0x182184, 0x18218B, 0x1821CE, 0x1821D5, 0x182270, 0x18227F, 0x18228A, 1),
    (0x278FBB, 0x18218D, 0x182194, 0x1821DB, 0x1821E2, 0x18222D, 0x18223C, 0x182247, 2),
)


def main() -> None:
    digest = hashlib.sha256(RUNTIME.read_bytes()).hexdigest()
    if digest != EXPECTED_SHA:
        raise ValueError("fixed original runtime changed")
    image = pefile.PE(str(RUNTIME))
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    evidence = {}

    def instruction(site: int):
        decoded = next(decoder.disasm(image.get_data(site, 16), base + site, count=1))
        evidence[hex(site)] = {"bytes": decoded.bytes.hex(),
                               "instruction": f"{decoded.mnemonic} {decoded.op_str}"}
        return decoded

    def expect(site: int, mnemonic: str, operands: str):
        insn = instruction(site)
        if (insn.mnemonic, insn.op_str) != (mnemonic, operands):
            raise ValueError(f"instruction changed at {site:#x}")
        return insn

    def target(site: int, mnemonic: str, expected: int):
        insn = instruction(site)
        if (insn.mnemonic != mnemonic or insn.operands[0].type != X86_OP_IMM
                or insn.operands[0].imm - base != expected):
            raise ValueError(f"control target changed at {site:#x}")

    def rip(site: int, operand_index: int, expected: int):
        insn = instruction(site)
        operand = insn.operands[operand_index]
        if (operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP
                or site + insn.size + operand.mem.disp != expected):
            raise ValueError(f"RIP target changed at {site:#x}")

    paths = []
    for flag, first, entry_jump, second, select_jump, clear, type_site, throw, payload in FLAGS:
        for check in (first, second):
            insn = instruction(check)
            if (insn.mnemonic != "cmp" or insn.operands[0].size != 1
                    or insn.operands[1].type != X86_OP_IMM or insn.operands[1].imm != 0):
                raise ValueError("flag check is not byte versus zero")
            rip(check, 0, flag)
        target(entry_jump, "jne", 0x18219B)
        target(select_jump, "jne", clear)
        clear_insn = instruction(clear)
        if (clear_insn.mnemonic != "mov" or clear_insn.operands[0].size != 1
                or clear_insn.operands[1].type != X86_OP_IMM or clear_insn.operands[1].imm != 0):
            raise ValueError("selected flag clear changed")
        rip(clear, 0, flag)
        type_instruction = instruction(type_site)
        if type_instruction.mnemonic != "lea" or type_instruction.op_str.split(",")[0] != "rdx":
            raise ValueError("throw type is not the second argument")
        rip(type_site, 1, 0x2543C8)
        target(throw, "call", 0x1CF9B8)
        expect(throw + 5, "int3", "")
        expect(throw - 4, "lea", "rcx, [rbp - 0x19]")
        paths.append({"flag_rva": hex(flag), "priority": len(paths) + 1,
                      "initial_check_rva": hex(first), "post_release_check_rva": hex(second),
                      "clear_rva": hex(clear), "throw_rva": hex(throw),
                      "throw_payload": payload, "throw_info_rva": "0x2543c8"})

    expect(0x182254, "mov", "dword ptr [rbp - 0x19], 0")
    expect(0x18225B, "mov", "byte ptr [rbp - 0x15], 0")
    expect(0x182277, "mov", "qword ptr [rbp - 0x19], 1")
    expect(0x182234, "mov", "qword ptr [rbp - 0x19], 2")
    expect(0x182158, "movups", "xmm6, xmmword ptr [rax]")
    expect(0x18215B, "movdqa", "xmm0, xmm6")
    expect(0x18215F, "psrldq", "xmm0, 8")
    expect(0x182164, "movd", "edi, xmm0")
    # With no control flags, xmm6 is returned without a decrement.
    expect(0x182196, "movups", "xmmword ptr [rbx], xmm6")
    target(0x182199, "jmp", 0x1821FB)
    # Control flags transfer ownership through one conditional result release.
    rip(0x18219B, 1, 0x278F98)
    expect(0x1821A2, "cmp", "edi, -0xb")
    target(0x1821A5, "jb", 0x1821C5)
    expect(0x1821A7, "movq", "rax, xmm6")
    expect(0x1821AC, "dec", "dword ptr [rax]")
    expect(0x1821AE, "cmp", "dword ptr [rax], 0")
    target(0x1821B1, "jg", 0x1821C5)
    expect(0x1821B3, "movdqa", "xmmword ptr [rbp - 9], xmm6")
    expect(0x1821B8, "lea", "rdx, [rbp - 9]")
    expect(0x1821BC, "mov", "rcx, qword ptr [rcx + 0x20]")
    target(0x1821C0, "call", 0xFBD80)
    # The selected flag is re-read after release; a disappeared flag yields tag 6.
    expect(0x1821E4, "mov", "qword ptr [rbp - 0x19], 0")
    expect(0x1821EC, "mov", "qword ptr [rbp - 0x11], 6")
    expect(0x1821F4, "movups", "xmm0, xmmword ptr [rbp - 0x19]")
    expect(0x1821F8, "movups", "xmmword ptr [rbx], xmm0")
    target(0x1821FF, "call", 0x7E80)

    throw_info = struct.unpack("<4I", image.get_data(0x2543C8, 16))
    if throw_info != (0, 0, 0, 0x254390):
        raise ValueError("throw info changed")
    count, catchable = struct.unpack("<2I", image.get_data(0x254390, 8))
    if (count, catchable) != (1, 0x2543A0):
        raise ValueError("catchable type list changed")
    type_record = struct.unpack("<7I", image.get_data(catchable, 28))
    if type_record != (0, 0x25DA50, 0, 0xFFFFFFFF, 0, 8, 0):
        raise ValueError("catchable type record changed")
    name = image.get_data(0x25DA60, 100).split(b"\0", 1)[0]
    if name != b".?AUGameFlowInterceptor@@":
        raise ValueError("control exception type changed")
    expect(0x1CFA36, "mov", "ecx, 0xe06d7363")
    expect(0x1CFA22, "mov", "edx, 1")
    rip(0x1CFA49, 0, 0x1FF320)
    image.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    imports = {entry.address - base: (module.dll.decode(), entry.name.decode() if entry.name else None)
               for module in image.DIRECTORY_ENTRY_IMPORT for entry in module.imports}
    if imports.get(0x1FF320) != ("KERNEL32.dll", "RaiseException"):
        raise ValueError("C++ throw helper RaiseException import changed")
    audit = {
        "runtime_sha256": digest, "evaluator_rva": "0x1820b0",
        "flags_in_selection_order": paths, "result_release": {
            "result_register": "xmm6", "tag_register": "edi",
            "condition": "unsigned tag >= 0xfffffff5; decrement referenced payload; call free helper if new count <= 0",
            "decrement_rva": "0x1821ac", "free_call_rva": "0x1821c0",
            "free_target_rva": "0xfbd80", "runtime_from_context_offset": "0x20",
            "rechecks_flags_after_release": True,
            "no_flag_path": "transfer result to caller without decrement",
            "all_flags_clear_after_release": "return payload 0 / tag 6"},
        "throw": {"helper_rva": "0x1cf9b8", "throw_info_rva": "0x2543c8",
                  "catchable_type_array_rva": "0x254390", "type_descriptor_rva": "0x25da50",
                  "type_name": name.decode(), "object_size": 8,
                  "payload_writes": "fb9 writes int32 discriminator 0 and byte at offset 4 = 0; ba/bb write full 8-byte values 1/2; remaining fb9 padding is not treated as initialized",
                  "exception_code": "0xe06d7363", "noncontinuable": True,
                  "raise_call_rva": "0x1cfa49", "raise_import": "KERNEL32.dll.RaiseException"},
        "instructions": evidence,
        "scope": "Exact fixed-binary evaluator consumer proof for all three flags: common conditional JS result release, subsequent ordered flag recheck, selected-flag-only clear, payload initialization, shared typed C++ throw, and no normal result delivery from each throw block. This closes M0-033's consumer paths. It does not establish producer semantics, natural trigger reachability, game-loop catch behavior, or resumed gameplay; those remain M0-032/M0-034."}
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: three control consumers, result release, RTTI and RaiseException targets verified")


if __name__ == "__main__":
    main()
