"""Capture and verify 250921 Load slot/short-record behavior in both runtimes."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1", "--maniac-version", "250921",
               "--log-file", "noop-battle-player.log"]
CASES = ([0, 7], [0, 7, 0], [0, 7, 0, 0], [1, 2, 0],
         [2, 2, 0], [91, -1, 0], [-3, 2, 0], [0, 9, 0])
SKIPPED = ([], [0], [1], [0, 0], [0, -1], [0, 666], [1, 0],
           [0, 0, 0], [0, 0, 0, 0], [2, -2, 0])


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture() -> None:
    for slot in (7, 9):
        fixture = ROOT / f"build/maniac-250921-load-seed{slot:02d}"
        subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                        "--save-probe", "--save-variant", "load-boundaries-seed",
                        "--load-seed-slot", str(slot), "--output", str(fixture)], check=True)
        shutil.copyfile(fixture / "manifest.json",
                        EVIDENCE / f"maniac-250921-load-seed{slot:02d}-manifest.json")
        subprocess.run([sys.executable, str(ROOT / "docs/probe-maniac-battle-noop.py"),
                        "--runtime", "original", "--save-variant", "load-boundaries-seed",
                        "--fixture", str(fixture), "--output",
                        str(EVIDENCE / f"maniac-250921-load-seed{slot:02d}-original-trace.json")], check=True)
    for index in range(len(CASES)):
        fixture = ROOT / f"build/maniac-250921-load-case{index}"
        subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                        "--save-probe", "--save-variant", "load-boundaries",
                        "--load-case", str(index), "--output", str(fixture)], check=True)
        manifest_path = fixture / "manifest.json"
        manifest = json.loads(manifest_path.read_text())
        for slot in (7, 9):
            seed = EVIDENCE / f"maniac-250921-load-seed{slot:02d}-original-trace.lsd"
            target = fixture / f"Save{slot:02d}.lsd"
            shutil.copyfile(seed, target)
            manifest["files"][target.name] = sha256(target)
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        shutil.copyfile(manifest_path, EVIDENCE / f"maniac-250921-load-case{index}-manifest.json")
        for runtime in ("original", "player"):
            output = EVIDENCE / f"maniac-250921-load-case{index}-{runtime}-trace.json"
            subprocess.run([sys.executable, str(ROOT / "docs/probe-maniac-battle-noop.py"),
                            "--runtime", runtime, "--save-variant", "load-boundaries",
                            "--fixture", str(fixture), "--output", str(output)], check=True)
            for suffix, slot in (("", 8), (".slot05", 5)):
                source = output.with_suffix(f"{suffix}.lsd")
                target = EVIDENCE / f"maniac-250921-load-case{index}-{runtime}-save{slot:02d}.lsd"
                source.replace(target)


def verify() -> None:
    if sha256(ORIGINAL) != ORIGINAL_SHA:
        raise ValueError("original runtime changed")
    helper = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    decoder = runpy.run_path(str(ROOT / "docs/verify-maniac-noop-long-skip.py"))
    def values(path: Path) -> list[int]:
        return decoder["saved_values"](path.read_bytes(), helper["ber"], helper["chunks"])
    seeds = {}
    for slot in (7, 9):
        seed = EVIDENCE / f"maniac-250921-load-seed{slot:02d}-original-trace.lsd"
        trace = json.loads((EVIDENCE / f"maniac-250921-load-seed{slot:02d}-original-trace.json").read_text())
        manifest_file = EVIDENCE / f"maniac-250921-load-seed{slot:02d}-manifest.json"
        manifest = json.loads(manifest_file.read_text())
        if (manifest["save_variant"] != "load-boundaries-seed" or
                manifest["expected_save_slot"] != slot or
                trace["fixture_manifest_sha256"] != sha256(manifest_file) or
                any(sha256(ROOT / f"build/maniac-250921-load-seed{slot:02d}" / name) != digest
                    for name, digest in manifest["files"].items())):
            raise ValueError(f"seed {slot} fixture changed")
        if (trace["runtime_sha256"] != ORIGINAL_SHA or trace["save_file_sha256"] != sha256(seed)
                or trace["save_slot"] != slot or values(seed)[0] != slot * 101):
            raise ValueError(f"seed {slot} changed")
        seeds[str(slot)] = sha256(seed)
    results = []
    for index, parameters in enumerate(CASES):
        manifest_file = EVIDENCE / f"maniac-250921-load-case{index}-manifest.json"
        manifest = json.loads(manifest_file.read_text())
        if (manifest["save_variant"] != "load-boundaries" or manifest["load_case"] != index
                or manifest["expected_save_slot"] != 8 or
                any(manifest["files"][f"Save{slot:02d}.lsd"] != seeds[str(slot)] for slot in (7, 9))):
            raise ValueError(f"case {index} fixture changed")
        fixture = ROOT / f"build/maniac-250921-load-case{index}"
        for name, digest in manifest["files"].items():
            if sha256(fixture / name) != digest:
                raise ValueError(f"case {index} input changed: {name}")
        selected = 9 if index == 7 else 7
        for runtime, binary, args in (("original", ORIGINAL, []), ("player", PLAYER, PLAYER_ARGS)):
            prefix = EVIDENCE / f"maniac-250921-load-case{index}-{runtime}"
            trace_file = prefix.with_name(prefix.name + "-trace.json")
            trace = json.loads(trace_file.read_text())
            first = prefix.with_name(prefix.name + "-save05.lsd")
            last = prefix.with_name(prefix.name + "-save08.lsd")
            if (trace["runtime_sha256"] != sha256(binary) or trace["runtime_arguments"] != args
                    or trace["fixture_manifest_sha256"] != sha256(manifest_file)
                    or trace["reference_sha256"] != ORIGINAL_SHA or trace["save_slot"] != 8
                    or trace["save_file_sha256"] != sha256(last)
                    or trace["intermediate_saves"]["5"]["sha256"] != sha256(first)
                    or trace["dispatch_events"]):
                raise ValueError(f"case {index} {runtime} trace changed")
            before = values(first)
            after = values(last)
            if before[:7] != [111, 7, 10, 0, 0, 0, 9]:
                raise ValueError(f"case {index} {runtime} skipped-record marker mismatch: {before}")
            if after[0] != selected * 101 or after[9] != 99:
                raise ValueError(f"case {index} {runtime} selected-slot/continuation mismatch: {after}")
            results.append({"case": index, "parameters": parameters, "runtime": runtime,
                            "preload_sha256": sha256(first), "postload_sha256": sha256(last),
                            "selected_slot": selected, "preload_markers": before[:7],
                            "postload_markers": [after[0], after[9]]})
    output = EVIDENCE / "maniac-250921-load-boundaries-audit.json"
    audit = {"original_sha256": ORIGINAL_SHA, "player_sha256": sha256(PLAYER),
             "seed_sha256": seeds, "skipped_parameters": SKIPPED, "cases": results,
             "scope": "Ten skipped zero/short/nonpositive/missing-slot commands, then eight direct, positive-global, negative-self, and arbitrary-nonzero-mode Load selections with two- through four-field records and nonempty ignored Text. Both runtimes first save skipped-command marker 10 and then restore the selected original seed's distinct 707/909 marker and continuation marker 99. Header bypass and A3 presentation remain separate."}
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: {len(CASES)} cases x 2 runtimes verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    args = parser.parse_args()
    if args.capture:
        capture()
    verify()
