"""Compare fixed original and Player opcode 1005 calls produced by opcode 3019."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
RUNNER = ROOT / "build/windows-x64-vs2022-release-local/Release/test_runner.exe"
PLAYER = RUNNER.with_name("Player.exe")
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1",
               "--log-file", "noop-battle-player.log"]
CONTEXTS = {"auto": (1, 62), "parallel": (0, 62),
            "nested": (1, 63), "battle": (2, 63)}


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture() -> None:
    generator = ROOT / "docs/generate-maniac-original-fixture.py"
    probe = ROOT / "docs/probe-maniac-battle-noop.py"
    for context in CONTEXTS:
        base = ROOT / "build" / f"maniac-common1005-{context}"
        variant = f"common1005-generated-{context}"
        fixture = ROOT / "build" / f"maniac-{variant}"
        if not fixture.resolve().is_relative_to((ROOT / "build").resolve()):
            raise ValueError(f"generated fixture path escaped the build directory: {fixture}")
        subprocess.run([sys.executable, str(generator), "--save-probe", "--save-variant",
                        f"common1005-{context}", "--output", str(base)], check=True)
        if fixture.exists():
            shutil.rmtree(fixture)
        shutil.copytree(base, fixture)
        env = dict(os.environ, MANIAC_COMMON1005_GENERATED_DIR=str(fixture))
        subprocess.run([str(RUNNER), "--no-colors",
                        "--test-case=Maniac 3019 generated 1005 fixture serialization"],
                       cwd=ROOT, env=env, check=True)
        manifest = json.loads((fixture / "manifest.json").read_text())
        manifest.update(save_variant=variant,
                        probe="3019 produces opcode 1005 calls with the original A0/A1 selections across four contexts",
                        verification_status="fixed original and Player generated-call differential")
        for name in manifest["files"]:
            manifest["files"][name] = sha(fixture / name)
        frozen = EVIDENCE / f"maniac-250921-{variant}-manifest.json"
        frozen.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        shutil.copyfile(frozen, fixture / "manifest.json")
        for runtime in ("original", "player"):
            command = [sys.executable, str(probe), "--runtime", runtime,
                       "--save-variant", variant, "--fixture", str(fixture), "--output",
                       str(EVIDENCE / f"maniac-250921-{variant}-{runtime}-trace.json")]
            if runtime == "original":
                command.append("--trace-common1005")
            subprocess.run(command, check=True)


def verify() -> None:
    prior = runpy.run_path(str(ROOT / "docs/verify-maniac-common1005.py"))
    parser = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    saved_values = runpy.run_path(str(ROOT / "docs/verify-maniac-noop-long-skip.py"))["saved_values"]
    checks = []
    for context, (mode, count) in CONTEXTS.items():
        variant = f"common1005-generated-{context}"
        fixture = ROOT / "build" / f"maniac-{variant}"
        frozen = EVIDENCE / f"maniac-250921-{variant}-manifest.json"
        manifest = json.loads(frozen.read_text())
        expected = prior["expected_values"](f"common1005-{context}")
        if (manifest["save_variant"] != variant or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA
                or manifest["expected_global_slots_after_event"] != expected):
            raise ValueError(f"wrong generated fixture {variant}")
        for name, digest in manifest["files"].items():
            if sha(fixture / name) != digest:
                raise ValueError(f"generated fixture changed {variant}/{name}")
        calls = []
        if context in ("nested", "battle"):
            calls.append(([3, 0xF0], 3))
        calls.extend((parameters[:2], selected) for parameters, selected in prior["RECORDS"])
        if len(calls) != count:
            raise ValueError("wrong generated call count")
        for runtime, executable, arguments in (("original", fixture / "RPG_RT.exe", []),
                                               ("player", PLAYER, PLAYER_ARGS)):
            prefix = EVIDENCE / f"maniac-250921-{variant}-{runtime}-trace"
            trace_path, save_path = prefix.with_suffix(".json"), prefix.with_suffix(".lsd")
            trace = json.loads(trace_path.read_text())
            if (trace["runtime_sha256"] != sha(executable)
                    or trace["runtime_arguments"][:3] + trace["runtime_arguments"][-2:] != arguments
                    or trace["fixture_manifest_sha256"] != sha(frozen)
                    or trace["save_file_sha256"] != sha(save_path)
                    or trace["save_file_length"] != save_path.stat().st_size):
                raise ValueError(f"generated trace provenance changed {variant}/{runtime}")
            numbers = saved_values(save_path.read_bytes(), parser["ber"], parser["chunks"])
            if numbers[:len(expected)] != expected or any(numbers[len(expected):]):
                raise ValueError(f"generated call results differ {variant}/{runtime}")
            if runtime == "original":
                records = trace["dispatch_events"]
                if not trace.get("common1005_instrumented") or len(records) != count:
                    raise ValueError(f"generated 1005 consumer not reached {variant}")
                if any(r["parameters"] != p or r["common_event_id"] != selected
                       or r["owner_mode"] != mode or r["frame_kind"] != 0x25
                       or r["depth_after"] != r["depth_before"] + 1
                       for r, (p, selected) in zip(records, calls)):
                    raise ValueError(f"generated 1005 consumer sequence differs {variant}")
            checks.append(dict(variant=variant, runtime=runtime, trace_sha256=sha(trace_path),
                               save_sha256=sha(save_path), consumer_calls=count,
                               variable_checksum=numbers[0]))
    output = EVIDENCE / "maniac-250921-common1005-generated-audit.json"
    output.write_text(json.dumps(dict(runner_sha256=sha(RUNNER), original_sha256=ORIGINAL_SHA,
        player_sha256=sha(PLAYER), cases=checks,
        scope="250921 Windows opcode 3019 inline literal production of 1005 across map autostart, map parallel, called common and battle lists. Native hook confirms generated parameter arrays enter the real 1005 consumer and append kind-37 frames; actual original/Player saves compare each call's cumulative outcome. This does not validate 3019's other construction modes or 1005 save/restart lifecycle."), indent=2) + "\n", encoding="utf-8")
    print(f"{output}: {len(checks)} runtime/context outcomes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    args = parser.parse_args()
    if args.capture:
        capture()
    verify()
