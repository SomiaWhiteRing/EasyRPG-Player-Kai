"""Bind the 250921 Save command branch inventory to the original executable."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-3002-scope-audit.json"
SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCE_HASHES = {
    "evidence/NativeBatch01-250921.md": "8724a645a1cd46b056a276915617b9810f4746e49fc7ea346831bcc04b18be94",
    "evidence/runtime-250921/00024f10.c": "387ab744ffb113fc0a55a89922bd4f61f9f47149326e542ff8cfd7cd770feb60",
    "analysis-work/reports/03002-e4a5aa9b6da20017481e8f5f036903ca2dfe7a3c3e2cacd78cafcc4a85241bf3.json": "7baf0dfc469b795997c8b034a352f20d519295d2a9f1de7cc95873f5412455c6",
}
INSTRUCTIONS = {
    0x4677B: "call 0x24f10",      # shared dispatch calls Save handler
    0x46780: "mov al, 1",         # no event yield from dispatch
    0x24F3B: "cmp edx, 1",       # one or zero argument boundary
    0x24F50: "je 0x24fb7",       # zero arguments skip slot evaluation
    0x24F55: "cmp dword ptr [rax], 0",  # direct/indirect A0
    0x24F60: "call 0x17d7c0",    # negative indirect source
    0x24F80: "cmp eax, ecx",     # positive global variable range
    0x24F8E: "test edi, edi",    # resolved slot <= 0 skips save
    0x24F92: "call 0xc8ec0",     # frequency protection
    0x24F97: "cmp edi, 0x10",    # last-slot update only below 16
    0x24F9C: "mov dword ptr [rip + 0x25569a], edi",
    0x24FA2: "inc dword ptr [rip + 0x255690]",  # save count
    0x24FAA: "call 0x82cb0",    # serialized save, no boolean result
    0x24FAF: "mov dword ptr [rsp + 0x58], 1",  # normal return result
    0x24FC7: "cmp ecx, 2",       # result flag absent in short record
    0x24FCF: "cmp dword ptr [rax + 8], 0",  # A2 result enabled
    0x24FD5: "cmp ecx, 3",       # A3 absent becomes zero
    0x24FDA: "mov ecx, dword ptr [rax + 0xc]",
    0x24FE5: "call 0x8c9e0",    # result writes after the save call
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    if digest(RUNTIME) != SHA256:
        raise ValueError("250921 original runtime changed")
    for name, expected in SOURCE_HASHES.items():
        if digest(SOURCE / name) != expected:
            raise ValueError(f"reviewed 3002 source changed: {name}")
    report = json.loads((SOURCE / next(name for name in SOURCE_HASHES
                                        if name.startswith("analysis-work/"))).read_text(encoding="utf-8"))
    if (report["runtime_trace"]["sha256"] != SHA256 or
            report["runtime_trace"]["rva"] != "0x24f10" or
            report["runtime_trace"]["end_rva"] != "0x25029"):
        raise ValueError("3002 runtime consumer report changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    observed = {}
    for rva, expected in INSTRUCTIONS.items():
        decoded = list(decoder.disasm(image.get_data(rva, 16), rva, count=1))
        if len(decoded) != 1:
            raise ValueError(f"missing 3002 instruction at {rva:#x}")
        actual = f"{decoded[0].mnemonic} {decoded[0].op_str}"
        if actual != expected:
            raise ValueError(f"3002 branch changed at {rva:#x}: {actual}")
        observed[f"0x{rva:x}"] = actual
    audit = {
        "original_runtime_sha256": SHA256,
        "reviewed_source_sha256": SOURCE_HASHES,
        "dispatch_and_handler_instructions": observed,
        "effective_branch_ids": [
            "3002:direct-slot", "3002:indirect-global-slot",
            "3002:indirect-negative-self-slot", "3002:nonpositive-or-missing-slot",
            "3002:result-disabled", "3002:result-enabled-signed-destination",
            "3002:result-enabled-missing-destination",
            "3002:positive-slot-save-count-last-slot-frequency",
        ],
        "parameter_rule": "A0=0 uses A1 directly; any nonzero A0 resolves A1 through the positive global variable or negative self accessor. Missing A1 resolves as zero. A2 requires at least three original arguments and a nonzero flag; A3 defaults to zero when absent. Additional arguments and Text are not read by this consumer.",
        "context_rule": "The shared 3002 dispatcher returns immediate success; map foreground, parallel, called common and battle event runners can reach it. A 3019-generated 3002 is an additional candidate production route requiring runtime verification. The save serializer observes the active runtime state, so checkpoint and restoration fidelity remain context dependent.",
        "remaining_questions": [
            "Player direct, indirect global and negative-self slot selection, invalid slots, and short records against fixed original traces",
            "Player save count, last-slot boundary and filename beyond two digits",
            "Frequency guard and exception/error behavior in the original and Player",
            "result-disabled and signed result-enabled delivery across event contexts, including battle, and restore continuation",
            "3019-generated 3002 argument, Text and caller-frame behavior",
            "full payload and cross-runtime save/load fidelity across additional frames and bank types",
            "241028 and Web/Android are separately scoped",
        ],
        "acceptance_ids": ["W11-008", "W11-009", "W11-015", "W11-016", "W11-017",
                           "W11-018", "W11-019", "W11-020", "W11-021", "W11-022"],
        "scope": "This is a 250921 Windows branch inventory and reviewed native control-flow binding. It is not Player feature acceptance. The existing W11-008/W11-009 evidence proves only their named result timing and negative self destination observations; battle snapshot and restoration also have separate acceptance conditions.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(audit['effective_branch_ids'])} branch groups bound to original 3002")


if __name__ == "__main__":
    main()
