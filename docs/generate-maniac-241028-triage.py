"""Align all 168 command consumers with the 241028/250921 native diff.

This is a screening list, not evidence of old-version runtime equivalence.
"""

from __future__ import annotations

from collections import Counter
import csv
import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REWRITER = ROOT.parent / "RPGRewriter-maniac"
DIFF = REWRITER / "analysis/Maniacs-Complete/native-version-diff.json"
INVENTORY = ROOT / "docs/maniac-250921-command-inventory.csv"
SUBMODES = ROOT / "docs/maniac-submode-ledger.csv"
OLD_EXE = REWRITER / "patch_maniacs_241028/English/RPG_RT.exe"
NEW_EXE = REWRITER / "patch_selfvar_250921/English/RPG_RT.exe"
OLD_SHA256 = "49ba424f3ff1fa7d4fc1334eea272fde4e437d07a6901dc6b87efb95383ec44d"
NEW_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
OUTPUT = ROOT / "docs/maniac-241028-consumer-triage.csv"
AUDIT = ROOT / "docs/evidence/maniac-241028-consumer-triage-audit.json"
FIELDS = [
    "opcode", "name", "work_package", "player_handler", "old_native_formatter",
    "new_native_formatter", "old_dispatch_status", "formatter_screen",
    "constant_index_delta", "dynamic_index_expression_delta", "screen_priority", "decision",
    "runtime_acceptance", "semantic_status",
]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def main() -> None:
    if sha256(OLD_EXE) != OLD_SHA256 or sha256(NEW_EXE) != NEW_SHA256:
        raise ValueError("241028 or 250921 original runtime changed")
    native = json.loads(DIFF.read_text(encoding="utf-8"))["commands"]
    inventory = read_csv(INVENTORY)
    submodes = read_csv(SUBMODES)
    if len(native) != 168 or len(inventory) != 168:
        raise ValueError("command inventory or native diff is incomplete")
    by_opcode = {str(entry["opcode"]): entry for entry in native}
    by_inventory = {entry["opcode"]: entry for entry in inventory}
    if len(by_opcode) != 168 or set(by_opcode) != set(by_inventory):
        raise ValueError("native diff and Player inventory disagree on opcodes")
    owners: dict[str, set[str]] = {}
    for entry in submodes:
        owners.setdefault(entry["opcode"], set()).add(entry["work_package"])
    if set(owners) != set(by_opcode) or any(len(value) != 1 for value in owners.values()):
        raise ValueError("submode work-package ownership is incomplete or ambiguous")

    rows = []
    for code in sorted(by_opcode, key=int):
        item = by_opcode[code]
        consumer = by_inventory[code]
        new_only = item["change"] == "new_declared_opcode"
        if not new_only and not (REWRITER / "analysis/Maniacs-Complete" / item["old_formatter"]).is_file():
            raise ValueError(f"missing old native evidence for {code}")
        if not (REWRITER / "analysis/Maniacs-Complete" / item["new_formatter"]).is_file():
            raise ValueError(f"missing new native evidence for {code}")
        added = item.get("added_observed_indices", [])
        removed = item.get("removed_observed_indices", [])
        index_delta = "+" + json.dumps(added) + " -" + json.dumps(removed) if added or removed else "none"
        dynamic_delta = ("changed" if item.get("old_dynamic_indices") != item.get("new_dynamic_indices")
                         else "none") if not new_only else "not_applicable"
        priority = ("P0_old_dispatch_absent" if new_only else
                    "P1_observed_index_delta" if added or removed else
                    "P2_dynamic_index_expression_delta" if dynamic_delta == "changed" else
                    "P3_local_formatter_delta" if item["change"] == "changed_local_syntax_review_required" else
                    "P4_same_local_formatter_helpers_unchecked")
        rows.append({
            "opcode": code,
            "name": consumer["name"],
            "work_package": next(iter(owners[code])),
            "player_handler": consumer["player_handler"] or consumer["player_dispatch"] or "not_recorded",
            "old_native_formatter": item.get("old_formatter", ""),
            "new_native_formatter": item["new_formatter"],
            "old_dispatch_status": "not_declared_in_dispatch_evidence" if new_only else "declared",
            "formatter_screen": item["change"],
            "constant_index_delta": index_delta,
            "dynamic_index_expression_delta": dynamic_delta,
            "screen_priority": priority,
            "decision": "old_version_runtime_diff_required",
            "runtime_acceptance": "W12-001",
            "semantic_status": "not_verified",
        })
        if code == "3004":
            # Shared flag chain was reviewed in the old runtime itself.
            rows[-1].update(
                decision="241028:3004-shared-flag-chain-dynamic-parity-open",
                runtime_acceptance="W12-013",
                semantic_status="static_shared_rule_decided_dynamic_parity_open",
            )
        if code == "3005":
            # Reviewed old runtime consumer, not the formatter screen above.
            # Keep the stable decision when the screening list is regenerated.
            rows[-1].update(
                decision="241028:3005-always-scaled-positive-global-only",
                runtime_acceptance="W12-011",
                semantic_status="static_consumer_decided_dynamic_parity_open",
            )
        if code == "3006":
            # Keep the reviewed old runtime decision alongside the screen.
            rows[-1].update(
                decision="241028:3006-scaled-positive-global-source-only",
                runtime_acceptance="W12-012",
                semantic_status="static_consumer_decided_dynamic_parity_open",
            )
    with OUTPUT.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    audit = {
        "old_runtime_sha256": OLD_SHA256,
        "new_runtime_sha256": NEW_SHA256,
        "native_diff_sha256": sha256(DIFF),
        "inventory_sha256": sha256(INVENTORY),
        "submode_ledger_sha256": sha256(SUBMODES),
        "triage_sha256": sha256(OUTPUT),
        "total_commands": len(rows),
        "formatter_screen": dict(Counter(row["formatter_screen"] for row in rows)),
        "screen_priority": dict(Counter(row["screen_priority"] for row in rows)),
        "constant_index_delta_count": sum(row["constant_index_delta"] != "none" for row in rows),
        "dynamic_index_expression_delta_count": sum(row["dynamic_index_expression_delta"] == "changed" for row in rows),
        "old_dispatch_absent_count": sum(row["old_dispatch_status"] != "declared" for row in rows),
        "semantic_status": "The 3004/3005/3006 rows link separate fixed-runtime consumer audits; all other rows remain formatter screens without old-runtime decisions.",
    }
    AUDIT.write_text(json.dumps(audit, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(audit, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
