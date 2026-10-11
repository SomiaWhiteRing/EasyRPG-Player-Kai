"""Reconcile original JS startup registration call sites with a live startup trace.

This audits the bounded initializer and its two host registration functions.
Unresolved call sites remain explicit instead of being counted as bindings.
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
TRACE = ROOT / "docs/evidence/maniac-250921-js-registration-trace.json"
MANIFEST = ROOT / "docs/evidence/maniac-250921-js-host-symbols-manifest.json"
BINDINGS = ROOT / "docs/evidence/maniac-250921-js-host-registration-audit.json"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-registration-reachability.json"
EXPECTED_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
FUNCTIONS = ((0x17E030, 0x17E575), (0x1803E0, 0x180CFB),
             (0x181650, 0x181856), (0x181856, 0x18199C),
             (0x18199C, 0x1819E1), (0x1819F0, 0x181C80))
HELPERS = {0x139DB0: "define_named_property", 0x101790: "define_property_by_atom",
           0xFAC60: "create_native_function"}
PARENT_CALLS = {0x18197D: 0x1803E0, 0x181982: 0x17E030}
# Each unobserved site is the type-6 side of a named host property
# assignment. The other side was reached in the fixed fresh startup.
# Name LEAs, the type test, both helper calls and the join are pinned below.
ALTERNATE_PROPERTY_SITES = (
    ("global.rm2k3", 0x18068A, 0x1806BF, 0x1806E5, 0x18071C, 0x1806EA, 0x180743),
    ("console.log", 0x1809EC, 0x180A21, 0x180A47, 0x180A7E, 0x180A4C, 0x180AA5),
    ("console.input", 0x180AF4, 0x180B29, 0x180B4B, 0x180B7E, 0x180B50, 0x180BA5),
    ("global.console", 0x180BB5, 0x180BEA, 0x180C0C, 0x180C3F, 0x180C11, 0x180C66),
    ("global.include", 0x1818C0, 0x1818FC, 0x181920, 0x181953, 0x181925, 0x181979),
)


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def main() -> None:
    if sha256(RUNTIME.read_bytes()) != EXPECTED_SHA256:
        raise ValueError("fixed 250921 original runtime changed")
    manifest_bytes = MANIFEST.read_bytes()
    trace = json.loads(TRACE.read_text(encoding="utf-8"))
    bindings = json.loads(BINDINGS.read_text(encoding="utf-8"))
    if (trace["runtime_sha256"] != EXPECTED_SHA256 or
            trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
            bindings["runtime_sha256"] != EXPECTED_SHA256):
        raise ValueError("live registration trace or direct bindings use a different original runtime")
    image = pefile.PE(str(RUNTIME))
    base = image.OPTIONAL_HEADER.ImageBase
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    def instruction(rva: int):
        decoded = list(decoder.disasm(image.get_data(rva, 16), base + rva, count=1))
        if len(decoded) != 1:
            raise ValueError(f"missing instruction at {rva:#x}")
        return decoded[0]

    alternate_properties = []
    for property_name, name_site, type_site, alternate_site, normal_site, jump_site, join_rva in ALTERNATE_PROPERTY_SITES:
        name = instruction(name_site)
        type_test = instruction(type_site)
        alternate = instruction(alternate_site)
        normal = instruction(normal_site)
        branch_join = instruction(jump_site)
        expected_name = property_name.rsplit(".", 1)[-1].encode("ascii") + b"\0"
        if (name.mnemonic != "lea" or name.operands[-1].type != X86_OP_MEM or
                name.operands[-1].mem.base != X86_REG_RIP or
                image.get_data(name_site + name.size + name.operands[-1].mem.disp,
                               len(expected_name)) != expected_name or
                type_test.mnemonic != "cmp" or type_test.op_str != "dword ptr [rax + 8], 6" or
                alternate.mnemonic != "call" or alternate.operands[0].type != X86_OP_IMM or
                alternate.operands[0].imm - base != 0x101790 or
                normal.mnemonic != "call" or normal.operands[0].type != X86_OP_IMM or
                normal.operands[0].imm - base != 0x101790 or
                branch_join.mnemonic != "jmp" or branch_join.operands[0].imm - base != join_rva):
            raise ValueError(f"alternate registration path changed: {property_name}")
        alternate_properties.append({
            "property": property_name,
            "name_lea_rva": f"0x{name_site:x}",
            "type_6_test_rva": f"0x{type_site:x}",
            "alternate_helper_call_rva": f"0x{alternate_site:x}",
            "fresh_startup_helper_call_rva": f"0x{normal_site:x}",
            "join_rva": f"0x{join_rva:x}",
        })
    calls = {}
    indirect = []
    for begin, end in FUNCTIONS:
        for instruction in decoder.disasm(image.get_data(begin, end - begin), base + begin):
            if instruction.mnemonic != "call":
                continue
            site = instruction.address - base
            operand = instruction.operands[0]
            if operand.type == X86_OP_IMM:
                target = operand.imm - base
                if target in HELPERS or site in PARENT_CALLS:
                    calls[site] = target
            elif operand.type == X86_OP_MEM and operand.mem.base == X86_REG_RIP:
                target = instruction.address + instruction.size + operand.mem.disp - base
                indirect.append({"call_site_rva": f"0x{site:x}", "iat_rva": f"0x{target:x}"})
            else:
                indirect.append({"call_site_rva": f"0x{site:x}", "operand": instruction.op_str})
    if any(calls.get(site) != target for site, target in PARENT_CALLS.items()):
        raise ValueError("initializer host registration calls changed")
    image.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"]])
    imports = {entry.address - base:
               f"{module.dll.decode(errors='replace')}.{entry.name.decode(errors='replace') if entry.name else entry.ordinal}"
               for module in image.DIRECTORY_ENTRY_IMPORT for entry in module.imports}
    for entry in indirect:
        if "iat_rva" not in entry or int(entry["iat_rva"], 16) not in imports:
            raise ValueError(f"non-import indirect call in audited initializer: {entry}")
        entry["import"] = imports[int(entry["iat_rva"], 16)]
    observed = {}
    for event in trace["events"]:
        site = int(event["call_site_rva"], 16)
        target = int(event["target_rva"], 16)
        if site in observed or calls.get(site) != target or target not in HELPERS:
            raise ValueError(f"live registration call is duplicate or outside audited static calls: {event}")
        observed[site] = target
    static_helpers = {site: target for site, target in calls.items() if target in HELPERS}
    if not observed or not set(observed).issubset(static_helpers):
        raise ValueError("live registration helper sites do not match static calls")
    unknown = sorted(set(static_helpers) - set(observed))
    if unknown != sorted(site[3] for site in ALTERNATE_PROPERTY_SITES):
        raise ValueError("an unobserved registration site is not attributed to a named property")
    audit = {
        "runtime_sha256": EXPECTED_SHA256,
        "fixture_manifest_sha256": sha256(manifest_bytes),
        "trace_sha256": sha256(TRACE.read_bytes()),
        "direct_binding_audit_sha256": sha256(BINDINGS.read_bytes()),
        "parent_registration_calls": [
            {"call_site_rva": f"0x{site:x}", "target_rva": f"0x{target:x}"}
            for site, target in sorted(PARENT_CALLS.items())],
        "registration_helper_calls": [
            {"call_site_rva": f"0x{site:x}", "target_rva": f"0x{target:x}",
             "helper": HELPERS[target], "observed_in_fixed_startup": site in observed}
            for site, target in sorted(static_helpers.items())],
        "indirect_import_calls": indirect,
        "static_helper_call_count": len(static_helpers),
        "observed_helper_call_count": len(observed),
        "static_sites_not_observed_in_one_startup": [f"0x{site:x}" for site in unknown],
        "alternate_property_paths": alternate_properties,
        "scope": "Fixed 250921 startup initializer and two host registration functions: all direct calls to three property/function registration helpers are enumerated; indirect calls in the initializer resolve to PE imports. The five helper sites not reached in one fresh startup are type-6 alternate assignments for five named properties, each rejoining its observed path. This attributes the sites, not the full provenance of standard-library descriptors or all registration calls, so M0-028 remains open.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(observed)}/{len(static_helpers)} helper sites observed in one startup; {len(unknown)} static sites remain")


if __name__ == "__main__":
    main()
