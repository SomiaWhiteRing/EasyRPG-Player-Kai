"""Bind opcode 1005's fixed 250921 consumer, helpers and branch inventory."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete"
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
OUTPUT = ROOT / "docs/evidence/maniac-250921-1005-scope-audit.json"
RUNTIME_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
SOURCE_HASHES = {
    "commands/01005.md": "4877d479a6a3e46d5e7c77d132ee032a3b0499b6619eaefc87abbf003a45a7b0",
    "evidence/NativeBatch01-250921.md": "8724a645a1cd46b056a276915617b9810f4746e49fc7ea346831bcc04b18be94",
    "evidence/runtime-250921/0001d550.c": "cd0aba7c831bd06ef34621a4b711528c97bdd0fb111f906f9f2525c5e40fa1d4",
    "analysis-work/reports/01005-13794fe54c231d11251d7d0d34805851bc219223fef4b4f2cbdbf1241eca28b3.json": "c96aa06b82d0473ee6a0aca1262f9d8ef75525d427c8d1a96a9d623d7b87b0e4",
}
SITES = {
    0x46A33: "call 0x1d550", 0x46A38: "mov al, 1",
    0x1D556: "cmp dword ptr [rcx], 0x2000", 0x1D55F: "jg 0x1d6ac",
    0x1D573: "cmp eax, 1", 0x1D583: "and edx, 0xf",
    0x1D58F: "cmp edx, 1", 0x1D59A: "call 0x17d7c0",
    0x1D5B1: "cmovs ecx, eax", 0x1D5DF: "call 0x17d7c0",
    0x1D603: "call 0x17d7c0", 0x1D638: "xor ebx, ebx",
    0x1D63E: "call 0x86e30", 0x1D64C: "call 0xb8bf0",
    0x1D651: "mov dword ptr [rdi + 0x2c], ebx",
    0x1D657: "mov byte ptr [rdi + 0x3d], 0x25",
    0x1D664: "jae 0x1d69d", 0x1D694: "mov qword ptr [rcx], rax",
    0x1D6A6: "call 0xc8f60", 0x1D6B3: "call 0x223d0",
}


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    if digest(RUNTIME) != RUNTIME_SHA:
        raise ValueError("fixed 250921 runtime changed")
    for path, expected in SOURCE_HASHES.items():
        if digest(SOURCE / path) != expected:
            raise ValueError(f"reviewed opcode 1005 source changed: {path}")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    report = json.loads((SOURCE / next(path for path in SOURCE_HASHES if path.startswith("analysis-work/"))).read_text(encoding="utf-8"))
    checked_helpers = []
    for window in report["inputs"]["disassembly"]:
        for instruction in window["instructions"]:
            rva = int(instruction["rva"], 16)
            decoded = next(decoder.disasm(image.get_data(rva, 16), image.OPTIONAL_HEADER.ImageBase + rva))
            if (decoded.bytes.hex() != instruction["bytes"]
                    or f"{decoded.mnemonic} {decoded.op_str}" != instruction["instruction"]):
                raise ValueError(f"reviewed helper instruction changed: {rva:#x}")
        checked_helpers.append({"first_rva": window["instructions"][0]["rva"],
                                "instructions": len(window["instructions"])})
    instructions = {}
    for rva, expected in SITES.items():
        decoded = next(decoder.disasm(image.get_data(rva, 16), rva))
        actual = f"{decoded.mnemonic} {decoded.op_str}"
        if actual != expected:
            raise ValueError(f"opcode 1005 changed at {rva:#x}: {actual}")
        instructions[f"0x{rva:x}"] = {"instruction": actual, "bytes": decoded.bytes.hex()}
    audit = {
        "runtime_sha256": RUNTIME_SHA,
        "reviewed_sources_sha256": SOURCE_HASHES,
        "reviewed_helper_windows": checked_helpers,
        "dispatch_and_handler_instructions": instructions,
        "handler_bytes_sha256": hashlib.sha256(image.get_data(0x1D550, 0x168)).hexdigest(),
        "effective_branch_ids": [
            "1005:literal-common-id", "1005:single-global-or-negative-self-read",
            "1005:double-global-or-negative-self-read", "1005:missing-or-invalid-id-error",
            "1005:zeroed-frame-append-and-bind-list", "1005:depth-over-8192-error",
        ],
        "parameter_rule": "A0 is the ID or variable reference. Missing A0 resolves zero. A1 low nibble 0 selects a literal, 1 a single numeric read, and every value 2..15 a double numeric read. Negative first references use self and force the first result to a nonpositive address before the second read. Zero/out-of-range references resolve zero. A1 high bits, extra Args and Text are ignored by the runtime and must be retained.",
        "frame_rule": "The handler checks old depth >8192 before evaluation; depth 8192 may append. It constructs a zeroed 0x40-byte frame, appends before ID validation, writes resolved ID at +0x2c and kind 0x25 at +0x3d, then binds a valid common-event command list. Caller arguments and return destinations are not copied. The shared dispatch returns success after push; subsequent execution is from the new stack top.",
        "applicable_contexts": ["map autostart", "map parallel", "called common event", "battle event page", "3019-produced command"],
        "context_evidence": "Shared dispatch and opcode 1005 consumer have no interpreter-mode guard. A battle editor command can therefore be encoded in any shared event list. Dynamic producer 3019 uses the shared consumer and needs separate context/production validation.",
        "remaining_questions": [
            "W02-023: valid ID modes, signed references, high bits, retained data and original/Player common-frame selection across static map/common/battle lists.",
            "W02-024: frame initialization, no caller argument/return copy, nesting and parent scheduling, wait/save/restart continuation and 3019-produced calls.",
            "W02-025: invalid/missing IDs, empty lists, depth 8192/8193 and error/cleanup ordering; any safe deviations need explicit evidence.",
            "241028 and Web/Android retain separate version/platform acceptance scopes.",
        ],
        "acceptance_ids": ["W02-023", "W02-024", "W02-025"],
        "scope": "250921 Windows opcode 1005 runtime branch scope only. Reviewed helpers and full consumer bytes are bound to the fixed original; no Player feature parity is inferred from the formatter or this static audit.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: six branch groups, three feature acceptance destinations")


if __name__ == "__main__":
    main()
