"""Audit directly registered 250921 QuickJS host names and callback addresses.

This is a static registration inventory, not a claim about callback semantics or
the exact QuickJS release. The registration functions are identified from the
initializer at RVA 0x181650 in the fixed English RPG_RT.exe.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP
import pefile


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-host-registration-audit.json"
EXPECTED_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
IMAGE_BASE = 0x140000000

# Each pair is a LEA of a NUL-terminated host name and a LEA of its callback.
# Shared callback anchors are intentional: the registered magic distinguishes
# numeric, switch and string access in the getter and setter tables.
BINDINGS = [
    ("global", "getv", 0x180466, 0x18047E, 0x17EDE0, 0, 0x180471),
    ("global", "gets", 0x1804AC, 0x18047E, 0x17EDE0, 1, 0x1804B7),
    ("global", "gett", 0x1804EB, 0x18047E, 0x17EDE0, 2, 0x1804F6),
    ("global", "setv", 0x18052A, 0x180542, 0x17F510, 0, 0x180535),
    ("global", "sets", 0x180570, 0x180542, 0x17F510, 1, 0x18057B),
    ("global", "sett", 0x1805AF, 0x180542, 0x17F510, 2, 0x1805BA),
    ("rm2k3", "getPicture", 0x18060C, 0x18062A, 0x180E20, None, None),
    ("Rm2k3_Picture", "id", 0x17E091, 0x17E0AF, 0x17E5D0, None, None),
    ("Rm2k3_Picture", "file", 0x17E0C0, 0x17E0CF, 0x17E6F0, None, None),
    ("Rm2k3_Picture", "scale", 0x17E0E5, 0x17E15D, 0x17EA80, None, None),
    ("Rm2k3_Picture", "show", 0x17E16B, 0x17E1AE, 0x17EBD0, None, None),
    ("Rm2k3_Picture", "move", 0x17E1BC, 0x17E1FF, 0x17ED10, None, None),
    ("Rm2k3_Picture", "erase", 0x17E20D, 0x17E240, 0x17EDB0, None, None),
    ("console", "log", 0x1809C4, 0x1809CB, 0x17FFC0, None, None),
    ("console", "input", 0x180ACC, 0x180AD3, 0x1801D0, None, None),
    ("global", "include", 0x181884, 0x181895, 0x1812F0, None, None),
]

# The file accessor has both getter and setter callbacks.
SECONDARY_CALLBACKS = [("Rm2k3_Picture", "file", 0x17E0DA, 0x17E8F0)]

ACCESSOR_DISPATCH = [
    ("getv", 0x17F3F8, 0x8C560),
    ("gets", 0x17F3CD, 0x8C070),
    ("gett", 0x17F214, 0x8CAC0),
    ("setv", 0x17F867, 0x8C9E0),
    ("sets", 0x17FF71, 0x8C100),
    ("sett", 0x17FC23, 0x8CCA0),
]


def main() -> None:
    digest = hashlib.sha256(RUNTIME.read_bytes()).hexdigest()
    if digest != EXPECTED_SHA256:
        raise ValueError("250921 English runtime changed")
    image = pefile.PE(str(RUNTIME), fast_load=True)
    if image.OPTIONAL_HEADER.ImageBase != IMAGE_BASE:
        raise ValueError("unexpected runtime image base")
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True

    def instruction(rva: int):
        return next(decoder.disasm(image.get_data(rva, 16), IMAGE_BASE + rva))

    def rip_target(rva: int) -> int:
        insn = instruction(rva)
        if insn.mnemonic != "lea" or insn.operands[-1].type != X86_OP_MEM or insn.operands[-1].mem.base != X86_REG_RIP:
            raise ValueError(f"expected RIP-relative LEA at {rva:#x}")
        return rva + insn.size + insn.operands[-1].mem.disp

    def call_target(rva: int) -> int:
        insn = instruction(rva)
        if insn.mnemonic != "call" or insn.operands[0].type != X86_OP_IMM:
            raise ValueError(f"expected direct call at {rva:#x}")
        return insn.operands[0].imm - IMAGE_BASE

    if call_target(0x18197D) != 0x1803E0 or call_target(0x181982) != 0x17E030:
        raise ValueError("initializer no longer calls host registration functions")
    top_level_names = {"rm2k3": 0x18068A, "console": 0x180BB5, "include": 0x181884}
    for name, anchor in top_level_names.items():
        target = rip_target(anchor)
        if image.get_data(target, len(name) + 1) != name.encode("ascii") + b"\0":
            raise ValueError(f"missing top-level host name: {name}")
    if rip_target(0x17E259) != 0x231BF0 or image.get_data(0x231BF0, 14) != b"Rm2k3_Picture\0":
        raise ValueError("picture class registration changed")

    records = []
    for group, name, name_anchor, callback_anchor, expected_callback, magic, magic_anchor in BINDINGS:
        name_rva = rip_target(name_anchor)
        callback_rva = rip_target(callback_anchor)
        if image.get_data(name_rva, len(name) + 1) != name.encode("ascii") + b"\0":
            raise ValueError(f"host name changed: {group}.{name}")
        if callback_rva != expected_callback:
            raise ValueError(f"callback changed: {group}.{name}")
        if magic_anchor is not None:
            insn = instruction(magic_anchor)
            if insn.mnemonic != "mov" or insn.operands[-1].type != X86_OP_IMM or insn.operands[-1].imm != ((magic << 16) | 3):
                raise ValueError(f"registration magic changed: {group}.{name}")
        records.append({
            "object": group, "name": name, "name_lea_rva": f"0x{name_anchor:x}",
            "name_rva": f"0x{name_rva:x}", "callback_lea_rva": f"0x{callback_anchor:x}",
            "callback_rva": f"0x{callback_rva:x}", "registration_magic": magic,
            "registration_magic_mov_rva": f"0x{magic_anchor:x}" if magic_anchor is not None else None,
        })
    secondary_callbacks = []
    for group, name, anchor, expected_callback in SECONDARY_CALLBACKS:
        actual_callback = rip_target(anchor)
        if actual_callback != expected_callback:
            raise ValueError(f"secondary host callback changed: {group}.{name}")
        secondary_callbacks.append({"object": group, "name": name,
                                    "callback_lea_rva": f"0x{anchor:x}",
                                    "callback_rva": f"0x{actual_callback:x}"})
    accessor_dispatch = []
    for name, call_site, expected_target in ACCESSOR_DISPATCH:
        actual_target = call_target(call_site)
        if actual_target != expected_target:
            raise ValueError(f"game storage call changed: {name}")
        accessor_dispatch.append({
            "name": name, "call_site_rva": f"0x{call_site:x}",
            "game_storage_target_rva": f"0x{actual_target:x}",
        })
    audit = {
        "runtime_sha256": digest,
        "initializer_rva": "0x181650",
        "host_registration_calls": {"0x18197d": "0x1803e0", "0x181982": "0x17e030"},
        "direct_top_level_names": top_level_names,
        "bindings": records,
        "secondary_callbacks": secondary_callbacks,
        "accessor_dispatch": accessor_dispatch,
        "scope": "Static direct-name/callback registration, including picture id getter and file getter/setter, plus six game-storage call sites. Picture descriptor fields are independently checked against live startup descriptors and native accessor names by the registration closure/dynamic-name audits. Runtime typeof and own-property probes place getv/gets/gett/setv/sets/sett globally and input under console; neither is an rm2k3 property. Array/proxy handling, coercion, exact QuickJS release, and control-exception lifecycle are not concluded. Registration reachability is audited separately.",
    }
    OUTPUT.write_text(json.dumps(audit, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(records)} directly named bindings")


if __name__ == "__main__":
    main()
