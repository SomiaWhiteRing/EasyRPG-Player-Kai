"""Run Player bank-import checks against frozen original Maniac LSD samples."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
RUNNER = ROOT / "build/windows-x64-vs2022-release-local/Release/test_runner.exe"
OUTPUT = EVIDENCE / "maniac-250921-original-import-audit.json"
audit = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))


def check(case: str, variable: str, path: Path) -> dict:
    env = os.environ.copy()
    env[variable] = str(path)
    completed = subprocess.run(
        [str(RUNNER), "--no-colors=true", "--test-case=" + case],
        cwd=ROOT, env=env, capture_output=True, text=True,
    )
    if completed.returncode or "Status: SUCCESS!" not in completed.stdout:
        raise RuntimeError(f"{path}:\n{completed.stdout}\n{completed.stderr}")
    return {"save": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            "case": case, "result": "passed"}


def main() -> None:
    variants = {name: audit["inspect"](name, *files) for name, files in audit["VARIANTS"].items()}
    checks = [check("Maniac original save fixture exposes serialized state",
                    "MANIAC_READ_ORIGINAL_SAVE", audit["SAVE"])]
    for name in ("map-self", "map-self-event2", "map-self-string", "map-self-strings",
                 "map-self-resume", "map-self-two-events", "map-self-two-maps", "map-switch"):
        path = audit["VARIANTS"][name][0]
        checks.append(check("Maniac original map bank samples import observed sparse strings",
                            "MANIAC_READ_ORIGINAL_BANK_SAMPLE", path))
    resume = audit["VARIANTS"]["map-self-resume"][0]
    checks.append(check("Maniac original self bank resumes the next variable command in Player",
                        "MANIAC_READ_ORIGINAL_RESUME_SAVE", resume))
    trace_path = EVIDENCE / "maniac-250921-save-map-self-resume-trace.json"
    trace = json.loads(trace_path.read_text(encoding="utf-8"))
    expected = [7, 7, 0, 42, 42]
    if not any(sample["slots"] == expected and sample["vector_length"] == 6
               for sample in trace["samples"]):
        raise ValueError("original save/resume process did not reach the expected after-save read")
    result = {
        "checks": checks,
        "original_resume_manifest_sha256": variants["map-self-resume"]["manifest_sha256"],
        "original_resume_trace_sha256": hashlib.sha256(trace_path.read_bytes()).hexdigest(),
        "original_after_save_physical_slots": expected,
        "player_after_load_global_id_4": 42,
        "map_switch_saved_party_map_id": variants["map-switch"]["saved_party_map_id"],
        "map_switch_saved_map_banks": variants["map-switch"]["map_self_numeric_vectors"],
        "scope": "Observed 250921 map/common bank fields, one map numeric command continuation, and bank persistence across an actual map transfer",
    }
    OUTPUT.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
