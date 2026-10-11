"""Verify Player self-bank identity in actual recursive and parallel scheduling."""

from __future__ import annotations

import hashlib
import json
import runpy
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
OUTPUT = EVIDENCE / "maniac-player-250921-self-scheduling-audit.json"
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
ORIGINAL_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_SHA256 = "6b4d1f70ece4819871ed8138226ac2e4c252aea1226553040ddb9d058f4ebe94"
ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921"]
EXPECTED = {
    "recursive-self-save": {3: 1, 4: 42, 5: 43},
    "parallel-self-save": {3: 42, 4: 43, 5: 42, 6: 43},
}


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def read_variables(save: bytes, ber, chunks) -> list[int]:
    length, position = ber(save, 0)
    if save[position:position + length] != b"LcfSaveData":
        raise ValueError("not an LSD save")
    root = chunks(save, position + length, len(save))
    system = next(entry for entry in root if entry["id"] == 101)
    fields = {entry["id"]: entry for entry in chunks(
        save, system["payload_offset"], system["payload_offset"] + system["length"])}
    values = fields[34]
    raw = save[values["payload_offset"]:values["payload_offset"] + values["length"]]
    if not raw or len(raw) % 4:
        raise ValueError("malformed numeric variable payload")
    return list(struct.unpack("<" + str(len(raw) // 4) + "i", raw))


def main() -> None:
    helpers = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    if sha256(ORIGINAL.read_bytes()) != ORIGINAL_SHA256 or sha256(PLAYER.read_bytes()) != PLAYER_SHA256:
        raise ValueError("runtime executables differ from fixed comparison")
    results = {}
    for variant, expected in EXPECTED.items():
        manifest_bytes = (EVIDENCE / f"maniac-250921-{variant}-manifest.json").read_bytes()
        manifest = json.loads(manifest_bytes)
        if (manifest["save_variant"] != variant or manifest["expected_save_slot"] != 1 or
                manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA256):
            raise ValueError(f"wrong fixture manifest: {variant}")
        fixture_dir = ROOT / "build" / f"maniac-original-{variant}"
        for name, expected_hash in manifest["files"].items():
            if sha256((fixture_dir / name).read_bytes()) != expected_hash:
                raise ValueError(f"fixture input changed: {variant} {name}")
        labels = {}
        for label, stem in (("original", "maniac-250921"), ("player", "maniac-player-250921")):
            trace_bytes = (EVIDENCE / f"{stem}-{variant}-trace.json").read_bytes()
            save_bytes = (EVIDENCE / f"{stem}-{variant}-save.lsd").read_bytes()
            trace = json.loads(trace_bytes.decode("utf-8-sig"))
            if (trace["reference_sha256"] != ORIGINAL_SHA256 or
                    trace["fixture_manifest_sha256"] != sha256(manifest_bytes) or
                    trace["save_file_sha256"] != sha256(save_bytes) or
                    trace["save_file_length"] != len(save_bytes) or trace["save_slot"] != 1):
                raise ValueError(f"trace/save mismatch: {label} {variant}")
            if label == "player" and (trace.get("player_sha256") != PLAYER_SHA256 or
                                      trace.get("player_arguments") != ARGS):
                raise ValueError(f"Player invocation mismatch: {variant}")
            if variant == "parallel-self-save" and trace.get("additional_enters_after_save") != 40:
                raise ValueError(f"parallel continuation input sequence changed: {label}")
            values = read_variables(save_bytes, helpers["ber"], helpers["chunks"])
            if any(len(values) < key or values[key - 1] != value for key, value in expected.items()):
                raise ValueError(f"self-bank observation differs: {label} {variant}: {values}")
            labels[label] = {
                "trace_sha256": sha256(trace_bytes),
                "save_sha256": sha256(save_bytes),
                "variables": {str(key): values[key - 1] for key in expected},
            }
        if labels["original"]["variables"] != labels["player"]["variables"]:
            raise ValueError(f"original/Player self-bank values differ: {variant}")
        results[variant] = {"manifest_sha256": sha256(manifest_bytes), "results": labels}
    audit = {
        "original_sha256": ORIGINAL_SHA256,
        "player_sha256": PLAYER_SHA256,
        "player_arguments": ARGS,
        "variants": results,
        "scope": "Fixed 250921 Windows original and Player projects: recursive common-event inner read 42 and outer read 43, and two parallel map events retain separate 42/43 self values on a later event pass. Save binary layouts differ; only the corresponding observable global variable IDs are compared. Other self-bank lifecycle paths remain separate acceptance items.",
    }
    OUTPUT.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{OUTPUT}: recursive and parallel self-bank observations match")


if __name__ == "__main__":
    main()
