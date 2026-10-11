"""Bind original startup target identities, lazy tables and residual static sites.

This closes target identity questions in the observed startup. It does not treat
an unexecuted generic property operation as unreachable from initialization.
"""

from __future__ import annotations

from bisect import bisect_right
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
import pefile


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
TRACE = EVIDENCE / "maniac-250921-js-registration-targets-trace.json"
PRIOR = EVIDENCE / "maniac-250921-js-registration-closure-audit.json"
ALTERNATES = EVIDENCE / "maniac-250921-js-registration-reachability.json"
MANIFEST = EVIDENCE / "maniac-250921-js-host-symbols-manifest.json"
LAZY_TRACE = EVIDENCE / "maniac-250921-js-lazy-registration-trace.json"
OUTPUT = EVIDENCE / "maniac-250921-js-registration-targets-audit.json"
EXPECTED_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
HELPERS = {0x139DB0, 0x101790, 0xFAC60}
EXPECTED_UNOBSERVED = {
    0x102A2E, 0x102BA7, 0x105151, 0x10FA2E, 0x11123C, 0x115496, 0x116D78,
    0x139C81, 0x14186A, 0x1423A6, 0x14255F, 0x142EFD, 0x145A91, 0x145CA2,
    0x14651A, 0x1466E3, 0x14FE81, 0x150E04, 0x150FD4, 0x1518B5, 0x151937,
    0x151F06, 0x152251, 0x152878, 0x152D0C, 0x153677, 0x153B88, 0x1548B9,
    0x154A4E, 0x155096, 0x1806E5, 0x180A47, 0x180B4B, 0x180C0C, 0x181920,
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def identity(value: dict) -> tuple[str, int]:
    return value["object_payload"], value["object_tag"]


def main() -> None:
    if sha256(RUNTIME) != EXPECTED_SHA:
        raise ValueError("fixed original binary changed")
    trace = json.loads(TRACE.read_text())
    prior = json.loads(PRIOR.read_text())
    alternatives = json.loads(ALTERNATES.read_text())
    lazy_trace = json.loads(LAZY_TRACE.read_text())
    manifest_sha = sha256(MANIFEST)
    if (trace["runtime_sha256"] != EXPECTED_SHA
            or trace["fixture_manifest_sha256"] != manifest_sha
            or prior["runtime_sha256"] != EXPECTED_SHA
            or alternatives["runtime_sha256"] != EXPECTED_SHA
            or lazy_trace["runtime_sha256"] != EXPECTED_SHA
            or lazy_trace["fixture_manifest_sha256"] != manifest_sha):
        raise ValueError("registration evidence provenance changed")
    image = pefile.PE(str(RUNTIME))
    base = image.OPTIONAL_HEADER.ImageBase
    live_base = int(trace["image_base"], 16)
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    for site, expected in {
        0x181799: "movups xmm6, xmmword ptr [rbx + 0x170]",
        0x170FD1: "movups xmmword ptr [rbx + 0x120], xmm6",
        0x165B7C: "movups xmmword ptr [rbx + 0x130], xmm6",
        0x17E51C: "add rax, qword ptr [rcx + 0x48]",
        0x17E523: "movups xmmword ptr [rax], xmm6",
        0x139BFC: "test al, al",
        0x139BFE: "je 0x140139d41",
        0x139C0C: "cmp al, 8",
        0x139C0E: "jne 0x140139da8",
        0x139C36: "mov rbx, qword ptr [rsi + 0x10]",
        0x139C3A: "movsxd rsi, dword ptr [rsi + 0x18]",
        0x139CA6: "add rbx, 0x20",
    }.items():
        instruction = next(decoder.disasm(image.get_data(site, 16), base + site, count=1))
        if f"{instruction.mnemonic} {instruction.op_str}" != expected:
            raise ValueError(f"target identity or delayed consumer instruction changed at {site:#x}")
    ranges = sorted((entry.struct.BeginAddress, entry.struct.EndAddress)
                    for entry in image.DIRECTORY_ENTRY_EXCEPTION)
    starts = [lo for lo, _ in ranges]

    def owner(site: int) -> tuple[int, int]:
        position = bisect_right(starts, site) - 1
        if position < 0 or not ranges[position][0] <= site < ranges[position][1]:
            raise ValueError(f"registration site has no PE function range: {site:#x}")
        return ranges[position]

    def direct_call(site: int, target: int) -> None:
        instruction = next(decoder.disasm(image.get_data(site, 5), base + site, count=1))
        if (instruction.mnemonic != "call" or instruction.size != 5
                or instruction.operands[0].type != X86_OP_IMM
                or instruction.operands[0].imm - base != target):
            raise ValueError(f"registration call changed at {site:#x}")

    def string(rva: int) -> str:
        data = image.get_data(rva, 160)
        if b"\0" not in data:
            raise ValueError(f"unterminated registration name at {rva:#x}")
        return data.split(b"\0", 1)[0].decode("utf-8")

    events = trace["events"]
    functions = trace["functions"]
    if (len(events) != 421 or len(functions) != 96
            or Counter(event["helper_rva"] for event in events) != {"0x139db0": 416, "0x101790": 5}):
        raise ValueError("fixed startup target observation counts changed")
    observed = {int(event["call_site_rva"], 16): int(event["helper_rva"], 16) for event in events}
    for function in functions:
        observed[int(function["call_site_rva"], 16)] = 0xFAC60
    if observed != {int(event["call_site_rva"], 16): int(event["target_rva"], 16)
                    for event in prior["calls"]}:
        raise ValueError("new target trace differs from prior 155-site startup set")
    for site, target in observed.items():
        direct_call(site, target)
    if len(trace["class_prototypes"]) != 87:
        raise ValueError("runtime class prototype count changed")
    labels: dict[tuple[str, int], list[str]] = defaultdict(list)

    def label(value: dict, name: str) -> None:
        if value["object_tag"] != -1:
            raise ValueError(f"expected object JSValue tag for {name}")
        labels[identity(value)].append(name)

    label(trace["global"], "global")
    label(trace["iterator_prototype"], "runtime.IteratorPrototype")
    label(trace["async_iterator_prototype"], "runtime.AsyncIteratorPrototype")
    for prototype in trace["class_prototypes"]:
        if prototype["object_tag"] == -1:
            label(prototype, f"runtime.class_prototype[{prototype['index']}]")
    for function in functions:
        if function["object_tag"] != -1 or not image.get_data(int(function["callback_rva"], 16), 1):
            raise ValueError("native function result/callback changed")
        label(function, f"native_function[{function['name']!r}]@{function['call_site_rva']}")
    for site, name in (("0x180771", "global.rm2k3"), ("0x180a7e", "global.console")):
        values = [event for event in events if event["call_site_rva"] == site]
        if len(values) != 1:
            raise ValueError(f"host root identity changed: {name}")
        label(values[0], name)
    if len(trace["objects"]) != 1 or trace["objects"][0]["class_id"] != 1:
        raise ValueError("TypedArray shared prototype creation changed")
    typed = trace["objects"][0]
    direct_call(0x17B291, 0xFA6F0)
    label(typed, "runtime.TypedArrayPrototype")
    if identity(typed["prototype"]) != identity(trace["class_prototypes"][1]):
        raise ValueError("TypedArray shared prototype parent changed")
    grouped: dict[tuple[str, int], list[dict]] = defaultdict(list)
    lazy_tables = {}
    for event in events:
        key = identity(event)
        if not labels[key]:
            raise ValueError(f"unattributed startup target: {event}")
        record = {"call_site_rva": event["call_site_rva"], "helper_rva": event["helper_rva"]}
        if event["helper_rva"] == "0x139db0":
            raw = bytes(event["descriptor_bytes"])
            if len(raw) != 32 or struct.unpack_from("<Q", raw)[0] - live_base != int(event["name_rva"], 16):
                raise ValueError("captured descriptor name pointer changed")
            name = string(int(event["name_rva"], 16))
            kind = raw[9]
            record.update(name=name, descriptor_type=kind,
                          getter_or_value_rva=event["descriptor_value_10_rva"],
                          callback_or_setter_rva=event["descriptor_value_18_rva"])
            if kind == 8:
                table = int(event["descriptor_value_10_rva"], 16)
                count = struct.unpack_from("<i", raw, 24)[0]
                descriptors = []
                for index in range(count):
                    entry = image.get_data(table + index * 32, 32)
                    entry_name = string(struct.unpack_from("<Q", entry)[0] - base)
                    entry_type = entry[9]
                    callback = struct.unpack_from("<Q", entry, 24)[0] - base if entry_type == 0 else None
                    if entry_type not in {0, 3, 6} or (callback is not None and not image.get_data(callback, 1)):
                        raise ValueError(f"unexpected lazy registration descriptor: {name}.{entry_name}")
                    descriptors.append({"name": entry_name, "type": entry_type,
                                        "callback_rva": f"0x{callback:x}" if callback is not None else None})
                lazy_tables[name] = {"table_rva": f"0x{table:x}", "count": count,
                                    "table_sha256": hashlib.sha256(image.get_data(table, count * 32)).hexdigest(),
                                    "descriptors": descriptors}
        else:
            record["atom"] = event["atom"]
        grouped[key].append(record)
    if {name: table["count"] for name, table in lazy_tables.items()} != {"Math": 44, "Reflect": 14, "JSON": 3, "Atomics": 13}:
        raise ValueError("lazy builtin descriptor inventory changed")
    realized_lazy = []
    if len(lazy_trace["events"]) != 25:
        raise ValueError("fixed lazy fixture observation count changed")
    for event in lazy_trace["events"]:
        name = string(int(event["name_rva"], 16))
        if event["object_tag"] != -1:
            raise ValueError(f"lazy creator did not produce object: {name}")
        if event["descriptor_type"] == 0:
            if event["callback_calls"] or not image.get_data(int(event["callback_rva"], 16), 1):
                raise ValueError(f"invalid delayed function callback: {name}")
            realized_lazy.append({"name": name, "descriptor_type": 0, "callback_rva": event["callback_rva"]})
            continue
        if event["descriptor_type"] != 8 or name not in lazy_tables:
            raise ValueError(f"unattributed delayed descriptor type: {name}")
        table = lazy_tables[name]
        if event["table_rva"] != table["table_rva"] or event["table_count"] != table["count"]:
            raise ValueError(f"delayed table differs from startup descriptor: {name}")
        actual = []
        for child in event["callback_calls"]:
            if identity(child) != identity(event) or child["call_site_rva"] != "0x139c81":
                raise ValueError(f"delayed table child targets another object: {name}")
            actual.append({"name": string(int(child["name_rva"], 16)), "type": child["descriptor_type"],
                           "callback_rva": child["callback_rva"]})
        if actual != table["descriptors"]:
            raise ValueError(f"dynamic delayed table callbacks differ from PE table: {name}")
        realized_lazy.append({"name": name, "descriptor_type": 8, "count": len(actual)})
    if {entry["name"] for entry in realized_lazy if entry["descriptor_type"] == 8} != {"JSON", "Reflect"}:
        raise ValueError("fixed symbol fixture delayed object set changed")
    picture = next(event for event in events if event["call_site_rva"] == "0x17e358")
    if identity(picture) != identity(trace["class_prototypes"][58]):
        raise ValueError("picture registration does not target class prototype 58")
    console = next(event for event in events if event["call_site_rva"] == "0x180a7e")
    if identity(console) != identity(next(event for event in events if event["call_site_rva"] == "0x180b7e")):
        raise ValueError("console input and log target different objects")
    global_sites = {"0x18071c", "0x1807c7", "0x180816", "0x180865", "0x1808b4",
                    "0x180903", "0x180952", "0x180c3f", "0x181953", "0x1817bd"}
    if any(identity(event) != identity(trace["global"]) for event in events if event["call_site_rva"] in global_sites):
        raise ValueError("host global targets changed")
    # Scan E8 operands, then validate candidate addresses against .pdata function
    # instruction boundaries. Whole-section linear decode stops on embedded data.
    static_sites = {}
    for section in image.sections:
        if not section.Characteristics & 0x20000000:
            continue
        blob = section.get_data()
        for offset, byte in enumerate(blob[:-4]):
            if byte != 0xE8:
                continue
            site = section.VirtualAddress + offset
            target = site + 5 + struct.unpack_from("<i", blob, offset + 1)[0]
            if target in HELPERS:
                static_sites[site] = target
    missing = set(static_sites) - set(observed)
    if len(static_sites) != 190 or missing != EXPECTED_UNOBSERVED:
        raise ValueError("complete PE direct-helper candidate inventory changed")
    alternate_sites = {int(path["alternate_helper_call_rva"], 16): path["property"]
                       for path in alternatives["alternate_property_paths"]}
    residual = []
    for site in sorted(missing):
        start, end = owner(site)
        instructions = list(decoder.disasm(image.get_data(start, end - start), base + start))
        position = next((index for index, instruction in enumerate(instructions)
                         if instruction.address - base == site), None)
        if position is None:
            raise ValueError(f"candidate is not at a function instruction boundary: {site:#x}")
        direct_call(site, static_sites[site])
        record = {"call_site_rva": f"0x{site:x}", "helper_rva": f"0x{static_sites[site]:x}",
                  "function_begin_rva": f"0x{start:x}", "function_end_rva": f"0x{end:x}",
                  "function_sha256": hashlib.sha256(image.get_data(start, end - start)).hexdigest()}
        if site in alternate_sites:
            record.update(classification="named-host-type6-alternate", target_property=alternate_sites[site],
                          remaining_question=None)
        elif site == 0x139C81:
            record.update(classification="lazy-type8-descriptor-table-loop",
                          target_property="new object receiving Math/Reflect/JSON/Atomics descriptor entries",
                          remaining_question=None)
        else:
            record.update(classification="generic-atom-property-operation",
                          argument_setup=[{"rva": f"0x{instruction.address-base:x}",
                                           "instruction": f"{instruction.mnemonic} {instruction.op_str}"}
                                          for instruction in instructions[max(0, position - 13):position]],
                          remaining_question="Trace parameter-supplied target/value source and initialization reachability; no fixed callback registration inferred")
        residual.append(record)
    audit = {"runtime_sha256": EXPECTED_SHA, "manifest_sha256": manifest_sha,
             "trace_sha256": sha256(TRACE), "prior_registration_audit_sha256": sha256(PRIOR),
             "alternate_path_audit_sha256": sha256(ALTERNATES),
             "lazy_registration_trace_sha256": sha256(LAZY_TRACE),
             "observed_property_calls": len(events), "observed_native_results": len(functions),
             "observed_call_sites": len(observed), "target_object_count": len(grouped),
             "target_objects": [{"identity_token": f"object-{index+1}", "labels": labels[key], "properties": properties}
                                for index, (key, properties) in enumerate(grouped.items())],
             "lazy_builtin_tables": lazy_tables, "static_direct_helper_sites": len(static_sites),
             "realized_lazy_registrations": realized_lazy,
             "native_function_results": [{"call_site_rva": function["call_site_rva"],
                                          "callback_rva": function["callback_rva"], "name": function["name"]}
                                         for function in functions],
             "unobserved_direct_helper_sites": residual,
             "remaining_questions": ["29 generic atom-property sites still need initialization reachability and target/value source attribution"],
             "scope": "All 52 target objects observed in one fixed startup are attributed to global/host roots, class prototype slots, iterator roots, TypedArray shared prototype creation or native function results. Four delayed builtin tables contain 74 PE-bound descriptors and callbacks; the fixed symbol fixture dynamically realizes JSON and Reflect with matching target objects and all 17 child descriptors. Type-8 table pointers/counts and the shared consumer loop are statically bound for all four tables. Full direct-helper candidate scan identifies 190 sites, including 35 not reached at startup. Five host alternates and one delayed descriptor loop are attributed; 29 generic property operations remain explicit. M0-028 remains open; this is original evidence only, not Player functionality."}
    if len(grouped) != 52 or sum(table["count"] for table in lazy_tables.values()) != 74:
        raise ValueError("target-object or lazy-descriptor counts changed")
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: 52 target objects, 74 lazy descriptors; 29 residual static sites retained")


if __name__ == "__main__":
    main()
