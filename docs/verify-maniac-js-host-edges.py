"""Verify fixed 250921 JavaScript host coercion, array/proxy and fault evidence."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import shutil
import struct
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "docs/evidence"
ORIGINAL = ROOT.parent / "RPGRewriter-maniac/patch_selfvar_250921/English/RPG_RT.exe"
SOURCE = ROOT / "docs/fixtures/maniac-js-host-edges.js"
REFERENCE_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
PREFIX = "maniac-250921-js-host-edges"
TYPES = ("number", "switch", "string")
SCALARS = ("undefined", "null", "false", "true", "fraction", "negative_fraction",
           "nan", "infinity", "wrap32", "numeric_string", "bad_string", "bigint",
           "symbol", "object", "throwing_object")
OTHERS = ("array/values", "array/range_descriptor", "array/range_pair", "array/range",
          "proxy/values", "proxy/range_descriptor", "error/value_conversion",
          "error/value_proxy_get", "order/id_before_value", "error/id_conversion",
          "error/revoked_values")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture() -> None:
    for selected in ("main", *TYPES):
        fixture = ROOT / "build" / ("maniac-original-js-host-edges" if selected == "main"
                                     else f"maniac-original-js-host-fault-{selected}")
        subprocess.run([sys.executable, str(ROOT / "docs/generate-maniac-original-fixture.py"),
                        "--save-probe", "--save-variant", "js-host-edges",
                        "--js-edge-case", selected, "--output", str(fixture)], check=True)
        name = PREFIX if selected == "main" else f"maniac-250921-js-host-fault-{selected}"
        shutil.copyfile(fixture / "manifest.json", EVIDENCE / f"{name}-manifest.json")
        if selected == "main":
            trace = EVIDENCE / f"{PREFIX}-trace.json"
            subprocess.run([sys.executable, str(ROOT / "docs/probe-maniac-battle-noop.py"),
                            "--runtime", "original", "--save-variant", "js-host-edges",
                            "--fixture", str(fixture), "--output", str(trace)], check=True)
            trace.with_suffix(".lsd").replace(EVIDENCE / f"{PREFIX}-save.lsd")
        else:
            subprocess.run([sys.executable, str(ROOT / "docs/probe-maniac-js-host-fault.py"),
                            "--fixture", str(fixture), "--output", str(EVIDENCE / f"{name}.json")],
                           check=True)


def verified_manifest(name: str, selected: str) -> tuple[dict, str]:
    path = EVIDENCE / f"{name}-manifest.json"
    manifest = json.loads(path.read_text(encoding="utf-8"))
    if (manifest["save_variant"] != "js-host-edges" or manifest["js_edge_case"] != selected
            or manifest["files"]["RPG_RT.exe"] != REFERENCE_SHA256):
        raise ValueError(f"wrong original fixture: {name}")
    selection = "" if selected == "main" else f"{selected}/error/id_proxy_get"
    expected_script = ("globalThis.__edge_case=" + json.dumps(selection) + ";\n"
                       + SOURCE.read_text(encoding="utf-8")).encode("utf-8")
    expected_script = expected_script.replace(b"\n", os.linesep.encode("ascii"))
    if hashlib.sha256(expected_script).hexdigest() != manifest["files"]["js-host-edges.js"]:
        raise ValueError(f"fixed JS source changed: {name}")
    fixture = ROOT / "build" / ("maniac-original-js-host-edges" if selected == "main"
                                 else f"maniac-original-js-host-fault-{selected}")
    if fixture.exists():
        for file_name, digest in manifest["files"].items():
            if sha256(fixture / file_name) != digest:
                raise ValueError(f"fixture input changed: {name}/{file_name}")
    return manifest, sha256(path)


def saved_values(path: Path) -> tuple[list[int], str, str]:
    helper = runpy.run_path(str(ROOT / "docs/analyze-maniac-original-save.py"))
    blob = path.read_bytes()
    size, position = helper["ber"](blob, 0)
    if blob[position:position + size] != b"LcfSaveData":
        raise ValueError("wrong save header")
    root = {entry["id"]: entry for entry in helper["chunks"](blob, position + size, len(blob))}
    system = root[101]
    fields = {entry["id"]: entry for entry in helper["chunks"](
        blob, system["payload_offset"], system["payload_offset"] + system["length"])}

    def payload(field: int) -> bytes:
        entry = fields[field]
        return blob[entry["payload_offset"]:entry["payload_offset"] + entry["length"]]

    numbers_raw = payload(34)
    numbers = list(struct.unpack("<" + "i" * (len(numbers_raw) // 4), numbers_raw))
    strings_raw = payload(36)
    units, position = helper["ber"](strings_raw, 0)
    result = strings_raw[position:position + units * 2].decode("utf-16-le")
    position += units * 2
    units, position = helper["ber"](strings_raw, position)
    last_case = strings_raw[position:position + units * 2].decode("utf-16-le")
    return numbers, result, last_case


def main() -> None:
    if sha256(ORIGINAL) != REFERENCE_SHA256:
        raise ValueError("original executable changed")
    main_manifest, main_digest = verified_manifest(PREFIX, "main")
    trace_path = EVIDENCE / f"{PREFIX}-trace.json"
    save_path = EVIDENCE / f"{PREFIX}-save.lsd"
    trace = json.loads(trace_path.read_text(encoding="utf-8"))
    if (trace["fixture_manifest_sha256"] != main_digest
            or trace["reference_sha256"] != REFERENCE_SHA256
            or trace["runtime_sha256"] != REFERENCE_SHA256
            or trace["runtime_arguments"] or trace["dispatch_events"]
            or trace["save_slot"] != main_manifest["expected_save_slot"]
            or trace["save_file_sha256"] != sha256(save_path)
            or trace["save_file_length"] != save_path.stat().st_size):
        raise ValueError("original JS edge save/trace provenance changed")
    numbers, result, last_case = saved_values(save_path)
    rows = json.loads(result)
    expected_path = EVIDENCE / f"{PREFIX}-expected.json"
    expected = json.loads(expected_path.read_text(encoding="utf-8"))
    expected_names = ([f"{kind}/scalar/{name}" for kind in TYPES for name in SCALARS]
                      + [f"{kind}/{name}" for kind in TYPES for name in OTHERS])
    if ([row["name"] for row in rows] != expected_names or rows != expected):
        raise ValueError("78 fixed original JS host observations changed")
    if numbers[:3] != [99, 78, 3] or last_case != "string/error/revoked_values":
        raise ValueError("JS continuation/checkpoint or diagnostic case changed")
    for row in rows:
        if set(row) != {"name", "outcome", "events", "after"}:
            raise ValueError("JS result shape changed")
        if row["name"].endswith("/error/id_proxy_get"):
            raise ValueError("native-fatal case must be captured separately")
    faults = {}
    for selected, label in (("number", "Variable"), ("switch", "Switch"),
                            ("string", "Strvar")):
        name = f"maniac-250921-js-host-fault-{selected}"
        _, manifest_digest = verified_manifest(name, selected)
        record = json.loads((EVIDENCE / f"{name}.json").read_text(encoding="utf-8"))
        dialog = record["native_dialog"]
        if (record["reference_sha256"] != REFERENCE_SHA256
                or record["runtime_sha256"] != REFERENCE_SHA256
                or record["runtime_arguments"] or record["fixture_manifest_sha256"] != manifest_digest
                or record["selected_case"] != f"{selected}/error/id_proxy_get"
                or dialog["api"] != "MessageBoxW" or dialog["caption"] != "RPG Maker 2003"
                or dialog["message"] != ("Rm2k3+MP Uncontrollable Error\rMessage: "
                                         f"Invalid {label} Access [4294967298]")
                or dialog["numeric_variables"][:4] != [0, 0, 1, 1]):
            raise ValueError(f"fixed native fault changed: {selected}")
        faults[selected] = dialog["message"]
    audit = {
        "reference_sha256": REFERENCE_SHA256,
        "source_sha256": sha256(SOURCE),
        "main_manifest_sha256": main_digest,
        "main_trace_sha256": sha256(trace_path),
        "main_save_sha256": sha256(save_path),
        "expected_results_sha256": sha256(expected_path),
        "main_cases": len(rows),
        "scalar_cases": len(TYPES) * len(SCALARS),
        "array_proxy_order_error_cases": len(TYPES) * len(OTHERS),
        "native_fault_dialogs": faults,
        "scope": "Fixed 250921 original getv/gets/gett and setv/sets/sett JS host cases establish scalar coercion, array endpoint/count reads, array/Proxy element access and conversion/error write order. Three ID-Proxy-get faults reach native dialogs. This is original evidence, not Player JS feature validation; neither fault continuation nor broader host behavior is asserted."
    }
    output = EVIDENCE / f"{PREFIX}-audit.json"
    output.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(f"{output}: {len(rows)} saved results and {len(faults)} native faults verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", action="store_true")
    args = parser.parse_args()
    if args.capture:
        capture()
    main()
