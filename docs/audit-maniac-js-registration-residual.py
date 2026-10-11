"""Map remaining property-helper calls in the fixed 250921 runtime.

The graph is deliberately an over-approximation of direct control transfers.
It does not resolve indirect calls, prove a branch is executable, or establish
that a property value is a native host callback.
"""

from __future__ import annotations

from bisect import bisect_right
from collections import Counter, defaultdict, deque
import hashlib
import json
from pathlib import Path
import re
import struct

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM
import pefile


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PRIOR = ROOT / "docs/evidence/maniac-250921-js-registration-targets-audit.json"
OUTPUT = ROOT / "docs/evidence/maniac-250921-js-registration-residual-audit.json"
EXPECTED_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
ROOTS = (0x181650, 0x1803E0, 0x17E030)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def hx(value: int) -> str:
    return f"0x{value:x}"


def main() -> None:
    if sha256(RUNTIME) != EXPECTED_SHA:
        raise ValueError("fixed original binary changed")
    prior = json.loads(PRIOR.read_text(encoding="utf-8"))
    if prior["runtime_sha256"] != EXPECTED_SHA:
        raise ValueError("prior property-helper audit changed runtime")
    residual = [item for item in prior["unobserved_direct_helper_sites"]
                if item["classification"] == "generic-atom-property-operation"]
    if len(residual) != 29 or {item["helper_rva"] for item in residual} != {"0x101790"}:
        raise ValueError("prior 29-site residual inventory changed")

    image = pefile.PE(str(RUNTIME))
    base = image.OPTIONAL_HEADER.ImageBase
    ranges = sorted((entry.struct.BeginAddress, entry.struct.EndAddress)
                    for entry in image.DIRECTORY_ENTRY_EXCEPTION)
    starts = [lo for lo, _ in ranges]
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True

    def owner(rva: int) -> int | None:
        index = bisect_right(starts, rva) - 1
        return starts[index] if index >= 0 and ranges[index][0] <= rva < ranges[index][1] else None

    inbound: dict[int, list[tuple[int, str, int]]] = defaultdict(list)
    outbound: dict[int, list[int]] = defaultdict(list)
    indirect: Counter[int] = Counter()
    decoded_bytes = 0
    for lo, hi in ranges:
        for instruction in decoder.disasm(image.get_data(lo, hi - lo), base + lo):
            decoded_bytes += instruction.size
            name = instruction.mnemonic
            if name != "call" and not name.startswith("j"):
                continue
            operand = instruction.operands[0]
            if operand.type == X86_OP_IMM:
                target = owner(operand.imm - base)
                if target is not None and target != lo:
                    inbound[target].append((instruction.address - base, name, lo))
                    outbound[lo].append(target)
            elif name in ("call", "jmp"):
                indirect[lo] += 1
    if decoded_bytes < 2_000_000 or len(ranges) != 6839:
        raise ValueError("PE function coverage changed")

    # Paths include all conditional arms and all direct edges. Presence of a
    # path is an upper bound on reachability, never proof of execution.
    paths: dict[int, list[int]] = {}
    queue = deque((root, [root]) for root in ROOTS)
    while queue:
        node, path = queue.popleft()
        if node in paths:
            continue
        paths[node] = path
        queue.extend((target, path + [target]) for target in outbound[node]
                     if target not in paths)

    sections = [(section.Name.rstrip(b"\0").decode("ascii", errors="replace"),
                 section.VirtualAddress, section.get_data()) for section in image.sections]
    functions: dict[int, dict] = {}
    for item in residual:
        site = int(item["call_site_rva"], 16)
        lo = int(item["function_begin_rva"], 16)
        hi = int(item["function_end_rva"], 16)
        if owner(site) != lo or hashlib.sha256(image.get_data(lo, hi - lo)).hexdigest() != item["function_sha256"]:
            raise ValueError(f"residual function changed at {site:#x}")
        call = next(decoder.disasm(image.get_data(site, 5), base + site, count=1))
        if (call.mnemonic != "call" or call.size != 5 or call.operands[0].type != X86_OP_IMM
                or call.operands[0].imm - base != 0x101790):
            raise ValueError(f"residual helper call changed at {site:#x}")
        atom_instructions = [entry["instruction"] for entry in item["argument_setup"]
                             if re.fullmatch(r"mov r8d, (?:0x[0-9a-f]+|[a-z0-9]+)", entry["instruction"])]
        atom_setup = atom_instructions[-1] if atom_instructions else None
        if lo not in functions:
            pointers = []
            needle = struct.pack("<Q", base + lo)
            for name, section_rva, blob in sections:
                offset = blob.find(needle)
                while offset != -1:
                    pointer = {"section": name, "rva": hx(section_rva + offset)}
                    if offset + 16 <= len(blob):
                        name_va = struct.unpack_from("<Q", blob, offset + 8)[0]
                        name_rva = name_va - base
                        if 0 <= name_rva < image.OPTIONAL_HEADER.SizeOfImage:
                            raw = image.get_data(name_rva, 96).split(b"\0", 1)[0]
                            if raw and len(raw) < 96 and all(0x20 <= byte < 0x7F for byte in raw):
                                pointer["adjacent_name"] = raw.decode("ascii")
                                pointer["adjacent_name_rva"] = hx(name_rva)
                    pointers.append(pointer)
                    offset = blob.find(needle, offset + 1)
            functions[lo] = {
                "function_begin_rva": hx(lo), "function_end_rva": hx(hi),
                "direct_graph_path_from_initializer": [hx(node) for node in paths[lo]] if lo in paths else None,
                "direct_inbound_edges": [
                    {"site_rva": hx(edge_site), "kind": kind, "source_function_rva": hx(source)}
                    for edge_site, kind, source in inbound[lo]
                ],
                "absolute_pointer_references": pointers,
                "indirect_call_or_jump_count": indirect[lo],
                "helper_calls": [],
            }
        functions[lo]["helper_calls"].append({"site_rva": hx(site), "atom_setup_in_local_window": atom_setup})

    sites_with_direct_path = sum(len(record["helper_calls"]) for record in functions.values()
                                 if record["direct_graph_path_from_initializer"] is not None)
    if len(functions) != 23 or sites_with_direct_path != 5:
        raise ValueError("residual direct-path classification changed")
    result = {
        "runtime_sha256": EXPECTED_SHA,
        "prior_target_audit_sha256": sha256(PRIOR),
        "initializer_roots_rva": [hx(root) for root in ROOTS],
        "pdata_function_count": len(ranges),
        "decoded_pdata_bytes": decoded_bytes,
        "direct_cross_function_edges": sum(len(edges) for edges in inbound.values()),
        "indirect_calls_or_jumps_in_pdata": sum(indirect.values()),
        "direct_graph_function_count": len(paths),
        "residual_helper_sites": len(residual),
        "residual_owner_functions": len(functions),
        "sites_in_functions_with_direct_graph_path": sites_with_direct_path,
        "sites_without_direct_graph_path": len(residual) - sites_with_direct_path,
        "functions": [functions[lo] for lo in sorted(functions)],
        "scope": "All 29 unobserved direct calls to generic atom-property helper 0x101790 are tied to 23 PE function ranges. The direct graph follows calls and jumps from three audited initializer functions, including both arms of conditions. Five sites lie in four functions with a direct graph path; 24 sites have no such path. This is an upper-bound graph only: it does not prove branch feasibility, resolve indirect dispatch, establish value/target provenance, or close M0-028.",
        "remaining_questions": [
            "Resolve indirect dispatch or callback-table paths into the 23 functions during initialization.",
            "Trace the object and property value through each candidate path; a generic atom-property helper call alone does not establish native host registration.",
        ],
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: 29 sites in 23 functions; {sites_with_direct_path} have a direct graph path")


if __name__ == "__main__":
    main()
