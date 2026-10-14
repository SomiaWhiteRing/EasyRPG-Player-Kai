"""Verify the fixed 250921 mode-2 End Command trace and battle save parity."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import runpy
import struct

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
NAME = "maniac-250921-end-command-battle"
MANIFEST = EVIDENCE / f"{NAME}-manifest.json"
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def variables(path: Path) -> list[int]:
    helper = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    blob = path.read_bytes()
    size, cursor = helper["ber"](blob, 0)
    if blob[cursor:cursor + size] != b"LcfSaveData":
        raise ValueError("wrong save header")
    root = {chunk["id"]: chunk for chunk in helper["chunks"](blob, cursor + size, len(blob))}
    system = root[101]
    fields = {chunk["id"]: chunk for chunk in helper["chunks"](
        blob, system["payload_offset"], system["payload_offset"] + system["length"])}
    entry = fields[34]
    raw = blob[entry["payload_offset"]:entry["payload_offset"] + entry["length"]]
    if len(raw) % 4:
        raise ValueError("unaligned variable vector")
    return list(struct.unpack("<" + "i" * (len(raw) // 4), raw))


def main() -> None:
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    fixture = ROOT / "build/maniac-end-command-battle"
    if manifest["save_variant"] != "end-command-battle" or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA:
        raise ValueError("wrong fixed original fixture")
    for name, digest in manifest["files"].items():
        if sha256(fixture / name) != digest:
            raise ValueError(f"fixture input changed: {name}")

    observed = {}
    for runtime in ("original", "player"):
        path = EVIDENCE / f"{NAME}-{runtime}-trace.json"
        trace = json.loads(path.read_text(encoding="utf-8"))
        expected_runtime = ORIGINAL_SHA if runtime == "original" else sha256(PLAYER)
        expected_arguments = [] if runtime == "original" else [
            "--new-game", "--patch-maniac", "1",
            "--log-file", "noop-battle-player.log"]
        save = path.with_suffix(".lsd")
        if (trace["runtime_sha256"] != expected_runtime
                or trace["fixture_manifest_sha256"] != sha256(MANIFEST)
                or trace["runtime_arguments"][:3] + trace["runtime_arguments"][-2:] != expected_arguments
                or trace["save_file_sha256"] != sha256(save)
                or trace["save_file_length"] != save.stat().st_size
                or trace["save_slot"] != 1):
            raise ValueError(f"{runtime} provenance mismatch")
        values = variables(save)
        if values[:10] != manifest["expected_global_slots_after_event"]:
            raise ValueError(f"{runtime} battle state mismatch: {values[:10]}")
        observed[runtime] = {"variables": values, "save_sha256": sha256(save),
                             "runtime_sha256": expected_runtime}

    trace = json.loads((EVIDENCE / f"{NAME}-original-trace.json").read_text(encoding="utf-8"))
    expected_stages = ["entry", "before_cleanup", "after_cleanup",
                       "first_update_call", "second_update_call"]
    if not trace.get("end_command_battle_instrumented") or len(trace["dispatch_events"]) != 1:
        raise ValueError("missing original mode-2 hook")
    event = trace["dispatch_events"][0]
    samples = event["events"]
    if (not event["reported"] or event["input_updates"] != 2
            or [sample["stage"] for sample in samples] != expected_stages
            or [sample["frame_depth"] for sample in samples] != [1, 1, 0, 0, 0]
            or [sample["cursor"] for sample in samples[:2]] != [4, 4]
            or [sample["opcode"] for sample in samples[:2]] != [0, 0]):
        raise ValueError(f"original mode-2 End Command path mismatch: {event}")

    audit = {"fixture_manifest_sha256": sha256(MANIFEST), "original_hook": event,
             "runtimes": observed,
             "result": "Mode-2 frame cleanup and two input updates observed in fixed original; Player battle-page variable slots agree. Player's extra trailing zero in the saved variable vector is tracked separately under save-format work."}
    output = EVIDENCE / f"{NAME}-audit.json"
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: original cleanup and two updates; battle marker slots agree")


if __name__ == "__main__":
    main()
