"""Capture and verify fixed-original/Player opcode 1005 error and depth boundaries."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
PLAYER = ROOT / "build/windows-x64-vs2022-release-local/Release/Player.exe"
RUNNER = PLAYER.with_name("test_runner.exe")
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
ORIGINAL_SHA = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
CASES = ("missing", "zero", "negative", "out-of-range", "int-max", "empty", "depth-pass", "depth-error")
IDS = {"missing": 0, "zero": 0, "negative": -1, "out-of-range": 2, "int-max": 2147483647}
PLAYER_ARGS = ["--new-game", "--patch-maniac", "1",
               "--log-file", "common1005-boundary.log"]
AGENT = r"""
const base = Process.mainModule.base;
const summary = {calls:0, appended:0, max_depth_before:0, max_depth_after:0,
                 returned:0, error:null, last_append:null};
let owner = null;
function frameState() {
    const depth = owner.readS32();
    if (!depth) return {depth, common_id:null, kind:null};
    const f = owner.add(8).readPointer().add((depth-1)*8).readPointer();
    return {depth, common_id:f.add(0x2c).readS32(), kind:f.add(0x3d).readU8()};
}
function variables() {
    const p = base.add(0x27a4b8).readPointer();
    const n = base.add(0x27a4c0).readPointer().sub(p).toInt32()/4;
    return Array.from({length:Math.min(n,3)},(_,i)=>p.add(i*4).readS32());
}
Interceptor.attach(base.add(0x1d550), {onEnter(args) {
    owner=args[0]; summary.calls++; summary.max_depth_before=Math.max(summary.max_depth_before,owner.readS32());
}, onLeave() {
    summary.returned++; summary.appended++; summary.last_append=frameState();
    summary.max_depth_after=Math.max(summary.max_depth_after,summary.last_append.depth);
}});
for (const [rva,kind] of [[0x1d6a6,'invalid-id'],[0x1d6b3,'depth-limit']]) {
    Interceptor.attach(base.add(rva), {onEnter() {
        if (kind === 'invalid-id') {
            summary.appended++; summary.last_append=frameState();
            summary.max_depth_after=Math.max(summary.max_depth_after,summary.last_append.depth);
        }
        summary.error={kind,frame:frameState(),variables:variables()};
        send(summary);
    }});
}
rpc.exports={snapshot() {return summary;}};
"""


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prefix(case: str) -> str:
    return f"maniac-250921-common1005-boundary-{case}"


def generate(case: str) -> Path:
    fixture = ROOT / "build" / f"maniac-common1005-boundary-{case}"
    subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                    "--save-probe", "--save-variant", "minimal", "--output", str(fixture)], check=True)
    env = dict(os.environ, MANIAC_COMMON1005_FIXTURE_DIR=str(fixture), MANIAC_COMMON1005_CASE=case)
    subprocess.run([str(RUNNER), "--no-colors", "--test-case=Maniac 1005 boundary fixture serialization"],
                   env=env, cwd=ROOT, check=True)
    manifest = json.loads((fixture / "manifest.json").read_text())
    success = case in ("empty", "depth-pass")
    manifest.update(save_variant="common1005-boundary", boundary_case=case,
                    probe="Opcode 1005 invalid ID, empty list, or recursive depth boundary",
                    expected_global_slots_after_event=[8192 if case == "depth-pass" else 0, 99] if success else None,
                    expected_global_numeric_vector_length_after_event=None,
                    expected_save_slot=1 if success else None,
                    verification_status="Fixed original and Player differential captured; verify with docs/verify-maniac-common1005-boundaries.py")
    for name in manifest["files"]:
        manifest["files"][name] = sha(fixture / name)
    frozen = EVIDENCE / f"{prefix(case)}-manifest.json"
    frozen.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    shutil.copyfile(frozen, fixture / "manifest.json")
    return fixture


def capture(case: str, runtime: str, fixture: Path) -> None:
    expected_error = case not in ("empty", "depth-pass")
    executable = fixture / "RPG_RT.exe" if runtime == "original" else PLAYER
    arguments = [] if runtime == "original" else PLAYER_ARGS
    save = fixture / "Save01.lsd"
    if save.exists(): save.unlink()
    output = EVIDENCE / f"{prefix(case)}-{runtime}.json"
    stderr = fixture / f"{runtime}-stderr.txt"
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    events, errors = [], []
    session = None
    with stderr.open("wb") as stream:
        process = subprocess.Popen([str(executable), *arguments], cwd=fixture, startupinfo=startup,
                                   stdout=stream, stderr=stream)
        try:
            script = None
            if runtime == "original":
                import frida
                session = frida.attach(process.pid)
                script = session.create_script(AGENT)
                def message(payload, _data):
                    (events if payload["type"] == "send" else errors).append(
                        payload["payload"] if payload["type"] == "send" else payload)
                script.on("message", message)
                script.load()
            deadline = time.monotonic() + 60
            observed_at = None
            last_digest = None
            while time.monotonic() < deadline:
                if expected_error:
                    observed = bool(events) if runtime == "original" else "Error:" in stderr.read_text(errors="replace")
                else:
                    current = sha(save) if save.exists() else None
                    observed = current is not None and current == last_digest
                    last_digest = current
                if observed:
                    if observed_at is None: observed_at = time.monotonic()
                    if time.monotonic() - observed_at > 0.6: break
                else:
                    observed_at = None
                if process.poll() is not None:
                    raise RuntimeError(f"{case}/{runtime} exited before capture: {process.returncode}")
                time.sleep(0.05)
            else:
                raise RuntimeError(f"{case}/{runtime} did not reach outcome: {errors}; {events}")
            native = script.exports_sync.snapshot() if script else None
            if errors: raise RuntimeError(f"native hook failed: {errors}")
            exit_code = process.poll()
            if exit_code is None:
                process.kill()
                process.wait()
            diagnostics = stderr.read_text(errors="replace")
            trace = dict(case=case, runtime=runtime, runtime_sha256=sha(executable),
                         runtime_arguments=arguments, manifest_sha256=sha(fixture / "manifest.json"),
                         native=native, save_exists=save.exists(), process_exit_code=exit_code,
                         error_lines=[line for line in diagnostics.splitlines() if "Error:" in line],
                         observation="No input. Read-only original hooks or Player diagnostic observed; harness terminated the process after the outcome. Error UI dismissal/process shutdown is not claimed.")
            if save.exists():
                target = output.with_suffix(".lsd")
                shutil.copyfile(save, target)
                trace["save_sha256"] = sha(target)
            output.write_text(json.dumps(trace, indent=2) + "\n", encoding="utf-8")
            print(f"{case}/{runtime}: captured {'error boundary' if expected_error else 'save after return'}", flush=True)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            if session:
                try: session.detach()
                except Exception: pass


def player_boundary_test() -> None:
    samples = EVIDENCE / "maniac-250921-common1005-boundary-player.tsv"
    env = dict(os.environ, MANIAC_COMMON1005_PLAYER_EVIDENCE=str(samples))
    result = subprocess.run([str(RUNNER), "--no-colors",
                             "--test-case=Maniac 250921 common call error and depth boundaries"],
                            env=env, cwd=ROOT, capture_output=True, text=True, check=True)
    evidence = dict(runner_sha256=sha(RUNNER), samples_sha256=sha(samples), output=result.stdout,
                    observation="Test-only log callback throws at fatal error entry, allowing inspection of the unmodified interpreter stack before error UI. Explicit Clear validates safe teardown; the handler does not resume after errors.")
    (EVIDENCE / "maniac-250921-common1005-boundary-player-test.json").write_text(
        json.dumps(evidence, indent=2) + "\n", encoding="utf-8")


def verify(cases: tuple[str, ...]) -> None:
    test = json.loads((EVIDENCE / "maniac-250921-common1005-boundary-player-test.json").read_text())
    samples = EVIDENCE / "maniac-250921-common1005-boundary-player.tsv"
    if test["runner_sha256"] != sha(RUNNER) or test["samples_sha256"] != sha(samples) or "SUCCESS!" not in test["output"]:
        raise ValueError("Player boundary test provenance changed")
    with samples.open(newline="") as stream:
        local = {row["case"]: row for row in csv.DictReader(stream, delimiter="\t")}
    helper = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    values = runpy.run_path(str(ROOT / "docs/verify-maniac-noop-long-skip.py"))["saved_values"]
    outcomes = []
    for case in cases:
        frozen = EVIDENCE / f"{prefix(case)}-manifest.json"
        manifest = json.loads(frozen.read_text())
        fixture = ROOT / "build" / f"maniac-common1005-boundary-{case}"
        expected_error = case not in ("empty", "depth-pass")
        if (manifest["boundary_case"] != case
                or manifest["files"]["RPG_RT.exe"] != ORIGINAL_SHA
                or manifest["expected_save_slot"] != (None if expected_error else 1)
                or manifest["expected_global_slots_after_event"] != (
                    None if expected_error else [8192 if case == "depth-pass" else 0, 99])):
            raise ValueError(f"wrong fixture: {case}")
        for name, digest in manifest["files"].items():
            if sha(fixture / name) != digest: raise ValueError(f"fixture changed: {case}/{name}")
        for runtime in ("original", "player"):
            path = EVIDENCE / f"{prefix(case)}-{runtime}.json"
            trace = json.loads(path.read_text())
            if (trace["manifest_sha256"] != sha(frozen)
                    or trace["runtime_sha256"] != (ORIGINAL_SHA if runtime == "original" else sha(PLAYER))
                    or trace["runtime_arguments"][:3] + trace["runtime_arguments"][-2:] != ([] if runtime == "original" else PLAYER_ARGS)):
                raise ValueError(f"provenance changed: {case}/{runtime}")
            if trace["save_exists"] == expected_error: raise ValueError("unexpected continuation/save")
            if not expected_error:
                save = path.with_suffix(".lsd")
                if sha(save) != trace["save_sha256"]: raise ValueError("save changed")
                actual = values(save.read_bytes(), helper["ber"], helper["chunks"])
                if actual[:2] != manifest["expected_global_slots_after_event"]: raise ValueError(f"wrong return values: {actual}")
            if runtime == "original":
                n = trace["native"]
                calls = 8193 if case == "depth-error" else 8192 if case == "depth-pass" else 1
                if n["calls"] != calls: raise ValueError(f"unexpected native calls: {case}: {n}")
                if expected_error:
                    depth = 8193 if case == "depth-error" else 2
                    event_id = 1 if case == "depth-error" else IDS[case]
                    if (n["error"]["kind"] != ("depth-limit" if case == "depth-error" else "invalid-id")
                            or n["error"]["frame"] != dict(depth=depth, common_id=event_id, kind=37)
                            or n["appended"] != (8192 if case == "depth-error" else 1)
                            or n["returned"] != (8192 if case == "depth-error" else 0)
                            or n["max_depth_before"] != (8193 if case == "depth-error" else 1)
                            or n["max_depth_after"] != depth
                            or n["error"]["variables"][:2] != [0, 8192 if case == "depth-error" else 0]):
                        raise ValueError(f"wrong error ordering: {case}: {n}")
                    row = local[case]
                    if (int(row["depth_after"]) != depth or int(row["common_id"]) != event_id
                            or int(row["kind"]) != 37 or not row["error"]):
                        raise ValueError(f"Player stack differs at error: {case}")
                else:
                    if (n["appended"] != calls or n["returned"] != calls
                            or n["max_depth_after"] != (8193 if case == "depth-pass" else 2)
                            or n["last_append"] != dict(depth=8193 if case == "depth-pass" else 2,
                                                         common_id=1, kind=37)
                            or n["error"] is not None):
                        raise ValueError(f"wrong successful unwind: {case}: {n}")
                    row = local[case]
                    if (int(row["depth_after"]) != (8193 if case == "depth-pass" else 0)
                            or row["error"] != "-"):
                        raise ValueError(f"Player return differs: {case}")
            elif expected_error:
                expected = "limit (8192)" if case == "depth-error" else f"invalid common event {IDS[case]}"
                if not any(expected in line for line in trace["error_lines"]): raise ValueError("Player diagnostic differs")
                if trace["native"] is not None: raise ValueError("Player claims original instrumentation")
            outcomes.append(dict(case=case, runtime=runtime, trace_sha256=sha(path), success=True))
    output = EVIDENCE / "maniac-250921-common1005-boundaries-audit.json"
    output.write_text(json.dumps(dict(cases=outcomes, player_test_sha256=sha(EVIDENCE / "maniac-250921-common1005-boundary-player-test.json"),
        scope="250921 Windows: missing/zero/negative/out-of-range/int-max IDs append a fresh kind-37 frame before nonreturning error; depth 8192 may append 8193 and unwind; depth 8193 errors without append; empty common lists pop and resume the caller. Fatal error presentation and process shutdown differ by platform and are not compared as game continuation."), indent=2) + "\n", encoding="utf-8")
    print(f"{output}: {len(outcomes)} runtime outcomes verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    parser.add_argument("--case", choices=CASES)
    args = parser.parse_args()
    selected = (args.case,) if args.case else CASES
    if args.capture:
        player_boundary_test()
        for case in selected:
            fixture = generate(case)
            for runtime in ("original", "player"): capture(case, runtime, fixture)
    verify(selected)
