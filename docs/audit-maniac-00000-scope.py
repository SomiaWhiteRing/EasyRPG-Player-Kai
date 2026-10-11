"""Audit 250921 End Command dispatch branches without claiming Player parity."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import pefile


ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
CONTRACT = ROOT.parent / "RPGRewriter-maniac/analysis/Maniacs-Complete/commands/00000.md"
TRACE = ROOT / "docs/evidence/maniac-250921-00000-end-command-trace.json"
OUTPUT = ROOT / "docs/evidence/maniac-250921-00000-scope-audit.json"
RUNTIME_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
CONTRACT_SHA256 = "c37e116ec27bdbf2e7b9fcd6cb3b0fb722608d36e90c72e42ff406bee95bb873"
SITES = {
    0x45703: "xor r15d, r15d",
    0x4762F: "cmp byte ptr [r13 + 0x38], r15b",
    0x47633: "jne 0x140047662",
    0x47635: "mov eax, dword ptr [r13]",
    0x47639: "cmp eax, 1",
    0x4763C: "jg 0x140047662",
    0x4763E: "test eax, eax",
    0x47640: "je 0x140047654",
    0x47642: "mov rax, qword ptr [r13 + 8]",
    0x4764B: "mov dword ptr [rcx + 0x24], r15d",
    0x4764F: "jmp 0x140046ef3",
    0x47662: "mov rcx, r13",
    0x47665: "call 0x1400454e0",
    0x4766A: "cmp byte ptr [r13 + 0x38], 2",
    0x4766F: "jne 0x140047d01",
    0x47675: "call 0x140096840",
    0x4767A: "call 0x140096840",
    0x4767F: "mov al, 1",
}
MANIFESTS = (
    ("map-autostart", "maniac-250921-js-host-negative-manifest.json"),
    ("map-parallel-common", "maniac-250921-noop-long-skip-manifest.json"),
    ("map-autostart-single", "maniac-250921-js-exception-continue-manifest.json"),
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    if sha256(RUNTIME) != RUNTIME_SHA256 or sha256(CONTRACT) != CONTRACT_SHA256:
        raise ValueError("250921 original runtime or reviewed End Command contract changed")
    source = CONTRACT.read_text(encoding="utf-8")
    if "branch_rva': '0x4762f'" not in source or "参数读取位置" not in source:
        raise ValueError("reviewed End Command dispatch index changed")
    image = pefile.PE(str(RUNTIME))
    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    block_digest = hashlib.sha256(image.get_data(0x4762F, 0x57)).hexdigest()
    if block_digest != "5f1ab096d6650efb05bb7716434ed0edf7ecb53edaec06a4434f7a0fff5fa10d":
        raise ValueError("complete End Command dispatch block changed")
    instructions = {}
    for rva, expected in SITES.items():
        decoded = list(decoder.disasm(image.get_data(rva, 16), image.OPTIONAL_HEADER.ImageBase + rva, count=1))
        if len(decoded) != 1:
            raise ValueError(f"missing End Command instruction at {rva:#x}")
        actual = f"{decoded[0].mnemonic} {decoded[0].op_str}"
        if actual != expected:
            raise ValueError(f"End Command branch changed at {rva:#x}: {actual}")
        instructions[f"0x{rva:x}"] = {"instruction": actual, "bytes": decoded[0].bytes.hex()}
    trace = json.loads(TRACE.read_text(encoding="utf-8"))
    if trace["reference_sha256"] != RUNTIME_SHA256 or len(trace["fixtures"]) != len(MANIFESTS):
        raise ValueError("wrong original End Command trace")
    observed = {}
    for fixture, (label, manifest_name) in zip(trace["fixtures"], MANIFESTS):
        manifest_path = ROOT / "docs/evidence" / manifest_name
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        if (fixture["context"] != label
                or fixture["fixture_manifest_sha256"] != sha256(manifest_path)
                or manifest["files"]["RPG_RT.exe"] != RUNTIME_SHA256):
            raise ValueError(f"End Command fixture provenance changed: {label}")
        events = fixture["events"]
        if not events or any(event["opcode"] != 0 or event["indent"] != 0
                             or event["frame_depth"] < 1 for event in events):
            raise ValueError(f"End Command trace not at opcode 0: {label}")
        observed[label] = [(event["stage"], event["owner_mode"], event["frame_depth"])
                           for event in events]
    required = {
        "map-autostart": [("entry", 1, 2), ("frame_cleanup", 1, 2)],
        "map-parallel-common": [("entry", 0, 2), ("frame_cleanup", 0, 2),
                                ("entry", 0, 1), ("single_frame_reset", 0, 1)],
        "map-autostart-single": [("entry", 1, 1), ("frame_cleanup", 1, 1)],
    }
    for label, subsequence in required.items():
        positions = iter(observed[label])
        if not all(any(item == expected for item in positions) for expected in subsequence):
            raise ValueError(f"End Command context/branch evidence missing: {label}")
    audit = {
        "runtime_sha256": RUNTIME_SHA256,
        "reviewed_contract_sha256": CONTRACT_SHA256,
        "trace_sha256": sha256(TRACE),
        "dispatch_block_sha256": block_digest,
        "dispatch_instructions": instructions,
        "effective_branch_ids": [
            "0:mode0-single-frame-cursor-reset",
            "0:mode0-nested-frame-cleanup",
            "0:mode1-frame-cleanup",
            "0:mode2-frame-cleanup-and-two-update-calls",
        ],
        "parameter_rule": "Opcode 0 is materialized from the four-byte LCF event-list terminator and has no encoded Args or Text. liblcf Code::END is opcode 10. The fixed opcode 0 dispatch block reads owner mode and frame depth. The zero-frame branch at 0x47654 dereferences null and is excluded as an invalid interpreter state, not an accepted mode.",
        "observed_contexts": ["map parallel single frame and nested common event",
                              "map autostart single frame and nested common event"],
        "candidate_contexts_to_verify": ["battle page mode 2 including cleanup and two 0x96840 calls",
                                         "other common/parallel and foreground frame transitions"],
        "remaining_questions": [
            "Original and Player cursor reset timing for mode 0 single-frame parallel execution",
            "Original and Player frame cleanup, bank identity and later command behavior for nested mode 0 and mode 1",
            "Mode 2 battle End Command reachability, two update calls, battle continuation and Player parity",
            "LCF terminator serialization is covered by W02-018/019/020; malformed/noncanonical zero records are excluded from standard event-list inputs",
            "Old 241028 consumer, Web and Android execution are separate acceptance scopes",
        ],
        "acceptance_ids": ["W02-018", "W02-019", "W02-020"],
        "scope": "250921 Windows End Command branch inventory. Fixed original runtime and reviewed contract identify four valid owner-mode/depth paths; read-only hooks observe three map paths. Battle mode 2 is statically identified, with runtime/Player verification left open. This is scope audit only, not command equivalence.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: {len(audit['effective_branch_ids'])} original End Command branch groups")


if __name__ == "__main__":
    main()
