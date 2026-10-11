"""Check that the shared-helper call audit still describes the current source."""

from __future__ import annotations

import csv
import hashlib
import json
import runpy
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
REFERENCES = DOCS / "maniac-shared-helper-references.csv"
CONSUMERS = DOCS / "maniac-shared-helper-consumers.csv"
OUTPUT = DOCS / "evidence/maniac-shared-helper-consumers-audit.json"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(path: Path) -> list[dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def main() -> None:
    generator = runpy.run_path(str(DOCS / "generate-maniac-ledger.py"))
    references = rows(REFERENCES)
    consumers = rows(CONSUMERS)
    if len(references) != 248 or len(consumers) != 209:
        raise ValueError("shared-helper audit coverage changed; review new references")
    kinds = Counter(row["reference_kind"] for row in references)
    if kinds["consumer_call"] != len(consumers):
        raise ValueError("consumer calls missing from audit")
    sources = {}
    source_hashes = {}
    for reference in references:
        path = ROOT / reference["file"]
        text = sources.setdefault(reference["file"], path.read_text(encoding="utf-8-sig"))
        line = text.splitlines()[int(reference["line"]) - 1]
        if line.strip() != reference["source_excerpt"]:
            raise ValueError(f"stale source reference: {reference['file']}:{reference['line']}")
        source_hashes[reference["file"]] = sha256(path)
    consumer_refs = [row for row in references if row["reference_kind"] == "consumer_call"]
    helper_call = generator["helper_call"]
    describe = generator["describe_helper_call"]
    for reference, consumer in zip(consumer_refs, consumers, strict=True):
        if any(consumer[key] != reference[source_key] for key, source_key in (
                ("file", "file"), ("line", "line"), ("helper", "helper"),
                ("owner_function", "owner_function"))):
            raise ValueError(f"consumer order/identity mismatch at {reference['file']}:{reference['line']}")
        invocation, args, _ = helper_call(sources[reference["file"]], int(reference["line"]), reference["helper"])
        mode, shift, operand, rule = describe(reference["helper"], args)
        if (consumer["call"], consumer["mode_source"], consumer["bitfield_nibble"],
                consumer["operand_source"], consumer["local_rule"]) != (invocation, mode, shift, operand, rule):
            raise ValueError(f"stale call interpretation: {reference['file']}:{reference['line']}")
        if not consumer["result_context"] or not consumer["review_boundary"]:
            raise ValueError(f"unreviewed consumer context: {reference['file']}:{reference['line']}")
        if not consumer["inventory_opcodes"] and "EasyRpg" not in consumer["command_handlers"]:
            raise ValueError(f"unmapped non-EasyRPG consumer: {reference['file']}:{reference['line']}")
    indirect = [row for row in consumers if row["owner_function"] == "Game_Interpreter::OperateValue"]
    expected_callers = {
        "Game_Interpreter::CommandChangeGold", "Game_Interpreter::CommandChangeItems",
        "Game_Interpreter::CommandChangeExp", "Game_Interpreter::CommandChangeLevel",
        "Game_Interpreter::CommandChangeParameters",
    }
    if len(indirect) != 1 or set(indirect[0]["command_handlers"].split(";")) != expected_callers:
        raise ValueError("OperateValue's indirect command consumers changed")
    interpreter_source = sources["src/game_interpreter.cpp"]
    if interpreter_source.count("OperateValue(") != len(expected_callers) + 1:
        raise ValueError("OperateValue's call sites changed")
    summary = {
        "scope": "Player source-level helper-call ownership, selector input, operand input, and result context; original per-command behavior remains unverified here",
        "reference_count": len(references),
        "reference_kinds": dict(sorted(kinds.items())),
        "consumer_count": len(consumers),
        "consumer_helpers": dict(sorted(Counter(row["helper"] for row in consumers).items())),
        "owner_functions": len({row["owner_function"] for row in consumers}),
        "command_handlers": len({handler for row in consumers for handler in row["command_handlers"].split(";")}),
        "inventory_mapped_consumers": sum(bool(row["inventory_opcodes"]) for row in consumers),
        "EasyRPG_extension_consumers": sum(not row["inventory_opcodes"] for row in consumers),
        "source_sha256": dict(sorted(source_hashes.items())),
        "references_sha256": sha256(REFERENCES),
        "consumers_sha256": sha256(CONSUMERS),
        "follow_up_risks": [
            "ValueOrVariable currently enables modes 2-4 for all callers; original commands may restrict individual modes (W01-007/W05-001).",
            "3020 assignment direct/indirect self string sources now use interpreter context; GetWithMode callers in conditional branch and string picture still need original comparison (W01-007/W06-001).",
            "Original behavior for malformed target modes remains unverified; Player now skips unsupported 3020 modes to avoid an uninitialized target (W04-001).",
        ],
    }
    OUTPUT.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
