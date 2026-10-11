"""Build an original Maniac runtime comparison project on Windows.

The doctest case writes LDB/LMT/LMU through liblcf. This wrapper supplies
the reference executable and a deterministic chipset bitmap, then records
hashes so subsequent runtime traces can identify the exact inputs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import zlib


ROOT = Path(__file__).resolve().parents[1]
REFERENCE = ROOT.parent / "RPGRewriter-maniac" / "patch_selfvar_250921" / "English" / "RPG_RT.exe"
REFERENCE_SHA256 = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
OLD_REFERENCE = ROOT.parent / "RPGRewriter-maniac" / "patch_maniacs_241028" / "English" / "RPG_RT.exe"
OLD_REFERENCE_SHA256 = "49ba424f3ff1fa7d4fc1334eea272fde4e437d07a6901dc6b87efb95383ec44d"
RUNNER = ROOT / "build" / "windows-x64-vs2022-release-local" / "Release" / "test_runner.exe"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def chunk(kind: bytes, payload: bytes) -> bytes:
    data = kind + payload
    return struct.pack(">I", len(payload)) + data + struct.pack(">I", zlib.crc32(data))


def chipset_png(width: int = 480, height: int = 256) -> bytes:
    pixel = bytes((38, 54, 68))
    rows = b"".join(b"\0" + pixel * width for _ in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, level=9))
        + chunk(b"IEND", b"")
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "maniac-original-fixture")
    parser.add_argument("--save-probe", action="store_true", help="Enable a separate runtime variant fixture")
    parser.add_argument("--runner", type=Path, default=RUNNER, help="Fixture serializer test_runner build")
    parser.add_argument("--save-variant", choices=("full", "minimal", "map-self", "map-self-event2", "map-self-string", "map-self-strings", "map-self-resume", "map-self-two-events", "map-self-two-maps", "map-switch", "title-cycle", "title-cycle-save", "js-title-cycle", "js-load-cycle", "load-overwrite", "parallel-self", "parallel-self-save", "recursive-self", "recursive-self-save", "legacy-controlvars", "legacy-negative-self", "string-self-source", "string-to-num-radix", "string-pop-line", "string-pop-line-range", "js-host-types", "js-host-accessors", "js-host-negative", "js-host-edges", "js-exception-continue", "js-host-inventory", "js-host-symbols", "reserved2-noop", "reserved-noop", "drawshape-noop", "noop-long-skip", "end-nest-long-skip", "end-nest-auto-common", "end-nest-battle", "opcode40-long-skip", "opcode40-auto-common", "opcode40-battle", "common1005-auto", "common1005-parallel", "common1005-nested", "common1005-battle", "common1005-wait-auto", "common1005-wait-parallel", "common1005-wait-nested", "common1005-wait-battle", "call12330-wait-auto", "call12330-wait-parallel", "call12330-wait-nested", "call12330-wait-battle", "end-command-battle", "end-command-cycle", "end-command-frames-auto", "end-command-frames-parallel", "noop-battle", "save-boundaries", "load-boundaries-seed", "load-boundaries", "load-resume", "load-export", "negative-result"), default="full")
    parser.add_argument("--reference-version", choices=("250921", "241028"), default="250921")
    parser.add_argument("--load-case", type=int, choices=range(8), default=0)
    parser.add_argument("--load-seed-slot", type=int, choices=(7, 9), default=7)
    parser.add_argument("--js-edge-case", choices=("main", "number", "switch", "string"), default="main")
    args = parser.parse_args()
    runner = args.runner.resolve()
    output = args.output.resolve()

    if not runner.is_file():
        raise SystemExit(f"Build the test_runner_player target first: {runner}")
    if runner.stat().st_mtime_ns < (ROOT / "tests/game_interpreter.cpp").stat().st_mtime_ns:
        raise SystemExit(f"test_runner is older than its fixture source; rebuild test_runner_player: {runner}")
    reference = OLD_REFERENCE if args.reference_version == "241028" else REFERENCE
    reference_sha256 = OLD_REFERENCE_SHA256 if args.reference_version == "241028" else REFERENCE_SHA256
    if args.reference_version == "241028" and args.save_variant not in {"legacy-controlvars", "legacy-negative-self", "js-exception-continue"}:
        raise SystemExit("The 241028 reference is currently supported only for legacy-controlvars, legacy-negative-self and js-exception-continue")
    if not reference.is_file() or sha256(reference) != reference_sha256:
        raise SystemExit(f"{args.reference_version} English reference executable is missing or changed: {reference}")

    output.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["MANIAC_WRITE_ORIGINAL_FIXTURE"] = str(output)
    env["MANIAC_ORIGINAL_LOAD_CASE"] = str(args.load_case)
    env["MANIAC_ORIGINAL_LOAD_SEED_SLOT"] = str(args.load_seed_slot)
    if args.save_variant == "js-host-edges":
        source = ROOT / "docs/fixtures/maniac-js-host-edges.js"
        selected = "" if args.js_edge_case == "main" else args.js_edge_case + "/error/id_proxy_get"
        (output / "js-host-edges.js").write_text("globalThis.__edge_case=" + json.dumps(selected) + ";\n" + source.read_text(), encoding="utf-8")
        env["MANIAC_ORIGINAL_JS_SOURCE"] = str(output / "js-host-edges.js")
    if args.save_probe:
        env["MANIAC_ORIGINAL_SAVE_PROBE"] = "1"
        if args.save_variant != "full":
            env["MANIAC_ORIGINAL_SAVE_VARIANT"] = args.save_variant
    subprocess.run(
        [str(runner), "--no-colors=true", "--test-case=Maniac original comparison fixture serializes deterministically"],
        cwd=ROOT,
        env=env,
        check=True,
    )
    shutil.copyfile(reference, output / "RPG_RT.exe")
    chipset = output / "ChipSet"
    chipset.mkdir(exist_ok=True)
    (chipset / "probe.png").write_bytes(chipset_png())
    if args.save_variant in {"noop-battle", "end-nest-battle", "opcode40-battle", "common1005-battle", "common1005-wait-battle", "call12330-wait-battle", "end-command-battle"}:
        for folder, width, height in (("Monster", 64, 64), ("Backdrop", 320, 160), ("BattleCharSet", 144, 384)):
            asset_dir = output / folder
            asset_dir.mkdir(exist_ok=True)
            (asset_dir / "probe.png").write_bytes(chipset_png(width, height))

    files = ["RPG_RT.exe", "RPG_RT.ldb", "RPG_RT.lmt", "Map0001.lmu", "ChipSet/probe.png"]
    if args.save_variant == "js-host-edges":
        files.append("js-host-edges.js")
    if args.save_variant in {"noop-battle", "end-nest-battle", "opcode40-battle", "common1005-battle", "common1005-wait-battle", "call12330-wait-battle", "end-command-battle"}:
        files.extend(["Monster/probe.png", "Backdrop/probe.png", "BattleCharSet/probe.png"])
    if args.save_variant == "map-switch":
        files.append("Map0002.lmu")
    reduced = args.save_probe and args.save_variant in {"minimal", "map-self", "map-self-event2", "map-self-string", "map-self-strings", "map-self-resume", "map-self-two-events", "map-self-two-maps", "map-switch", "title-cycle", "title-cycle-save", "js-title-cycle", "js-load-cycle", "load-overwrite", "parallel-self", "parallel-self-save", "recursive-self", "recursive-self-save", "legacy-controlvars", "legacy-negative-self", "string-self-source", "string-to-num-radix", "string-pop-line", "string-pop-line-range", "js-host-types", "js-host-accessors", "js-host-negative", "js-host-edges", "js-exception-continue", "js-host-inventory", "js-host-symbols", "reserved2-noop", "reserved-noop", "drawshape-noop", "noop-long-skip", "end-nest-long-skip", "end-nest-auto-common", "end-nest-battle", "opcode40-long-skip", "opcode40-auto-common", "opcode40-battle", "common1005-auto", "common1005-parallel", "common1005-nested", "common1005-battle", "common1005-wait-auto", "common1005-wait-parallel", "common1005-wait-nested", "common1005-wait-battle", "call12330-wait-auto", "call12330-wait-parallel", "call12330-wait-nested", "call12330-wait-battle", "end-command-battle", "end-command-cycle", "end-command-frames-auto", "end-command-frames-parallel", "noop-battle", "save-boundaries", "load-boundaries-seed", "load-boundaries", "load-resume", "load-export"}
    negative_result = args.save_probe and args.save_variant == "negative-result"
    manifest = {
        "reference": "patch_maniacs_241028 English" if args.reference_version == "241028" else "patch_selfvar_250921 English",
        "probe": ("Run 13 standalone 3002 commands with zero through four arguments, invalid slots, disabled/enabled results, nonzero slot mode, self slot source and nonempty ignored Text; save final markers in slot 9"
                  if args.save_variant == "save-boundaries" else
                  "Start troop 1 battle after setting variable 10=110; its variable-gated page runs each of 3024/3034/3035 with zero, one and three parameters, writes markers 11..99 and saves in battle"
                  if args.save_variant == "noop-battle" else
                  "Execute 10050 End Nest records on battle page, then save adjacent marker"
                  if args.save_variant == "end-nest-battle" else
                  "Autostart parent scans skipped End Nest branches, calls common event with 10050 End Nest records, and saves parent continuation"
                  if args.save_variant == "end-nest-auto-common" else
                  "Execute 10050 alternating 3024/3035 records in parallel event 1, call a common event containing 3024/3034/3035, then let parallel event 2 snapshot and save"
                  if args.save_variant == "noop-long-skip" else
                  "Execute standalone 3035 with nonempty Args/Text, then set global variable 1 to 99 and save"
                  if args.save_variant == "drawshape-noop" else
                  "Execute standalone 3034 with nonempty Args/Text, then set global variable 1 to 99 and save"
                  if args.save_variant == "reserved-noop" else
                  "Execute standalone 3024 with nonempty Args/Text, then set global variable 1 to 99 and save"
                  if args.save_variant == "reserved2-noop" else
                  "Enumerate own string and symbol keys of original JavaScript host roots, picture instance/prototype, and registered functions"
                  if args.save_variant == "js-host-symbols" else
                  "Set JavaScript global to 42, save, mutate it to 43, load, and observe the restored event continuation"
                  if args.save_variant == "js-load-cycle" else
                  "Evaluate JavaScript before/after a global assignment, return to title, and begin a new game twice"
                  if args.save_variant == "js-title-cycle" else
                  "Save map self before/after write, return to title, and begin a new game twice"
                  if args.save_variant == "title-cycle-save" else
                  "Read map self before/after write, return to title, and begin a new game twice"
                  if args.save_variant == "title-cycle" else
                  "Save map self 42, mutate to 43, load the save, and read the restored self bank"
                  if args.save_variant == "load-overwrite" else
                  "Two parallel map events expose separate self banks through repeated saved global reads"
                  if args.save_variant == "parallel-self-save" else
                  "Recursive common event exposes shared inner/outer self bank through a saved map result"
                  if args.save_variant == "recursive-self-save" else
                  "Two parallel map events retain separate numeric self banks across updates"
                  if args.save_variant == "parallel-self" else
                  "Recursive common event reads and writes the same numeric self bank in inner and outer frames"
                  if args.save_variant == "recursive-self" else
                  "ControlVars writes global variable 1=7, copies it to 2, then saves"
                  if args.save_variant == "legacy-controlvars" else
                  "ControlVars writes map self -1=42, copies it into global variable 1, writes marker variable 2=99, then saves"
                  if args.save_variant == "legacy-negative-self" else
                  "3036 writes current map self string -1, 3020 reads it directly/indirectly and a literal into global strings 1/3/2, then saves"
                  if args.save_variant == "string-self-source" else
                  "3020 assigns 08 and 0x10, converts using decimal and explicit hexadecimal modes, then saves numeric results"
                  if args.save_variant == "string-to-num-radix" else
                  "3020 pops A-CR-LF-B into two destinations, pops an aliased single-line source and appends its result to Y, then saves"
                  if args.save_variant == "string-pop-line" else
                  "3020 pops the first line of each string in range 1..3 into destination array 5..7, then saves"
                  if args.save_variant == "string-pop-line-range" else
                  "3030 evaluates typeof for host objects and functions in rm2k3 and global scopes, then saves"
                  if args.save_variant == "js-host-types" else
                  "3030 writes global variable, switch and string with host accessors, reads them again, then saves"
                  if args.save_variant == "js-host-accessors" else
                  "3030 throws a JavaScript exception, then evaluates another script, writes a ControlVars marker and saves"
                  if args.save_variant == "js-exception-continue" else
                  "3030 lists own properties of globalThis, rm2k3, console and any global picture constructor, then saves"
                  if args.save_variant == "js-host-inventory" else
                  "3036/10220 storage followed by 12330 common selectors 0/3/4/5/6/7 and all three offset sources, with 12310 numeric/switch direct/indirect returns"),
        "expected_global_slots_after_event": ([0, 1, 0, 0, 0, 0, 99, 0, 0, 42] if args.save_variant == "js-host-accessors" else [0, 99] if args.save_variant in {"string-pop-line", "string-pop-line-range", "js-host-types"} else [0, 8, 0, 16, 99] if args.save_variant == "string-to-num-radix" else [0, 0, 99, -1] if args.save_variant == "string-self-source" else [0, 0 if args.reference_version == "241028" else 42, 99] if args.save_variant == "legacy-negative-self" else [0, 7, 7] if args.save_variant == "legacy-controlvars" else [0, 0, 0, 1, 42, 43] if args.save_variant == "recursive-self" else [0, 0, 0, 42, 43, 42, 43] if args.save_variant == "parallel-self" else [0, 0, 0, 42, 43] if args.save_variant == "load-overwrite" else [0, 0, 0, 0, 42] if args.save_variant == "title-cycle" else [0, 7, 0, 42, 43] if args.save_variant == "load-export" else [0, 7, 0, 42, 42] if args.save_variant == "load-resume" else [7, 7, 0, 42, 42] if args.save_variant == "map-self-resume" else [7, 7] if reduced else
            [7, 7, 42, 42, 99, 0, 99, 0, 0, 3, 2, 3, 2, 77, 70, 0, 2, 88, 1, 1, 4, 1, 1, 1, 23, 66, 67, 0, 68, 69, 1, 70, 3, 1, 3, 33, 3] + ([1] if args.save_probe else [])),
        "expected_global_numeric_vector_length_after_event": 11 if args.save_variant == "js-host-accessors" else 2 if args.save_variant in {"string-pop-line", "string-pop-line-range", "js-host-types"} else 5 if args.save_variant == "string-to-num-radix" else 4 if args.save_variant == "string-self-source" else 8 if args.save_variant == "parallel-self" else 7 if args.save_variant == "recursive-self" else 6 if args.save_variant in {"map-self-resume", "title-cycle", "load-overwrite", "load-resume", "load-export"} else 3 if reduced else (39 if negative_result else (38 if args.save_probe else 37)),
        "expected_global_switch_slots_after_event": [0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1] if args.save_variant == "js-host-accessors" else [0] if reduced else [0, 2, 3, 1, 1],
        "expected_global_switch_vector_length_after_event": 11 if args.save_variant == "js-host-accessors" else 1 if reduced else 5,
        "verification_status": ("inputs serialized; run probe-maniac-original-save-file.ps1 and verify-maniac-241028-controlvars.py"
                                if args.save_variant == "legacy-controlvars" else
                                "inputs serialized; run probe-maniac-original-save-file.ps1 and verify-maniac-241028-negative-self.py"
                                if args.save_variant == "legacy-negative-self" else
                                "inputs serialized; run probe-maniac-original-save-file.ps1 and inspect global/self strings"
                                if args.save_variant == "string-self-source" else
                                "inputs serialized; run probe-maniac-original-save-file.ps1 and inspect decimal/hex numeric values"
                                if args.save_variant == "string-to-num-radix" else
                                "inputs serialized; run probe-maniac-original-save-file.ps1 and inspect CRLF, no-LF and alias strings"
                                if args.save_variant == "string-pop-line" else
                                "inputs serialized; run probe-maniac-original-save-file.ps1 and inspect range PopLine strings"
                                if args.save_variant == "string-pop-line-range" else
                                "inputs serialized; run probe-maniac-original-save-file.ps1 and inspect JavaScript host types"
                                if args.save_variant == "js-host-types" else
                                "inputs serialized; run probe-maniac-original-save-file.ps1 and inspect JavaScript host accessor writes"
                                if args.save_variant == "js-host-accessors" else
                                "inputs serialized; run probe-maniac-original-slot-zero.ps1 and the doctest case for runtime observations"),
        "save_probe": args.save_probe,
        "save_variant": args.save_variant,
        "files": {name: sha256(output / name) for name in files},
        "fixture_runner_sha256": sha256(runner),
        "fixture_source_sha256": sha256(ROOT / "tests/game_interpreter.cpp"),
        "fixture_generator_sha256": sha256(Path(__file__)),
    }
    if args.save_variant == "js-exception-continue":
        manifest["expected_global_slots_after_event"] = [0, 0, 42, 99]
        manifest["expected_global_numeric_vector_length_after_event"] = 4
        manifest["verification_status"] = "inputs serialized; run probe-maniac-original-save-file.ps1 and inspect exception continuation"
    if args.save_variant == "js-host-inventory":
        manifest["expected_global_slots_after_event"] = [0, 99]
        manifest["expected_global_numeric_vector_length_after_event"] = 2
        manifest["verification_status"] = "inputs serialized; run probe-maniac-original-save-file.ps1 and inspect host property names"
    if args.save_variant == "js-host-symbols":
        manifest["expected_global_slots_after_event"] = [0, 99]
        manifest["expected_global_numeric_vector_length_after_event"] = 2
        manifest["verification_status"] = "inputs serialized; run probe-maniac-original-save-file.ps1 and inspect host own symbols and nested keys"
    if args.save_variant == "reserved2-noop":
        manifest["expected_global_slots_after_event"] = [0, 99]
        manifest["expected_global_numeric_vector_length_after_event"] = 2
        manifest["verification_status"] = "inputs serialized; run probe-maniac-original-save-file.ps1 and verify 3024 advances to the marker"
    if args.save_variant == "reserved-noop":
        manifest["expected_global_slots_after_event"] = [0, 99]
        manifest["expected_global_numeric_vector_length_after_event"] = 2
        manifest["verification_status"] = "inputs serialized; run probe-maniac-original-save-file.ps1 and verify 3034 advances to the marker"
    if args.save_variant == "drawshape-noop":
        manifest["expected_global_slots_after_event"] = [0, 99]
        manifest["expected_global_numeric_vector_length_after_event"] = 2
        manifest["verification_status"] = "inputs serialized; run probe-maniac-original-save-file.ps1 and verify 3035 advances to the marker"
    if args.save_variant == "noop-long-skip":
        manifest["expected_global_slots_after_event"] = [0, 99, 99, 33, 44, 55]
        manifest["expected_global_numeric_vector_length_after_event"] = 6
        manifest["verification_status"] = "inputs serialized; compare first parallel-event snapshot in original and Player saves"
    if args.save_variant == "end-nest-long-skip":
        manifest.update(expected_save_slot=1, expected_global_slots_after_event=[99, 99],
                        expected_global_numeric_vector_length_after_event=None,
                        probe="10050 encoded END10 markers with retained Args/Text before a map-parallel first-tick snapshot",
                        verification_status="inputs serialized; compare original and Player first-tick saves")
    if args.save_variant == "opcode40-long-skip":
        manifest.update(expected_save_slot=1, expected_global_slots_after_event=[99, 99],
                        expected_global_numeric_vector_length_after_event=None,
                        probe="10050 encoded opcode 40 records with retained Args/Text before a map-parallel first-tick snapshot",
                        verification_status="inputs serialized; compare original and Player first-tick saves")
    if args.save_variant == "end-command-cycle":
        manifest.update(expected_save_slot=2,
                        expected_global_slots_after_event=[2, 2],
                        expected_global_numeric_vector_length_after_event=None,
                        probe="Parallel event increments global/self counters, saves consecutive rounds and ends at the file terminator",
                        verification_status="inputs serialized; compare original and Player repeat cycles")
    if args.save_variant in {"end-command-frames-auto", "end-command-frames-parallel"}:
        manifest.update(expected_save_slot=2,
                        expected_global_slots_after_event=[2, 2, 2],
                        expected_global_numeric_vector_length_after_event=None,
                        probe="Map parent calls common event 3; separate self banks and parent continuation advance together across two End Command rounds",
                        verification_status="inputs serialized; compare original frame cleanup and both runtime saves")
    if args.save_variant == "save-boundaries":
        manifest["expected_global_slots_after_event"] = [3, 1, 0, 0, 0, 88, 88, 1, 99, 1]
        manifest["expected_global_numeric_vector_length_after_event"] = 10
        manifest["expected_save_slot"] = 9
        manifest["verification_status"] = "inputs serialized; compare original and Player short save records, slot selection and invalid-result markers"
    if args.save_variant == "noop-battle":
        manifest["expected_global_slots_after_event"] = [11, 22, 33, 44, 55, 66, 77, 88, 99, 110]
        manifest["expected_global_numeric_vector_length_after_event"] = 10
        manifest["verification_status"] = "inputs serialized; compare original and Player battle-event saves"
    if args.save_variant == "end-nest-battle":
        manifest.update(expected_save_slot=1, expected_global_slots_after_event=[99, 0, 0, 0, 0, 0, 0, 0, 0, 110],
                        expected_global_numeric_vector_length_after_event=None,
                        probe="Battle page executes 10050 End Nest records then a marker and Save01",
                        verification_status="inputs serialized; compare original and Player battle-event save")
    if args.save_variant == "opcode40-battle":
        manifest.update(expected_save_slot=1, expected_global_slots_after_event=[99, 0, 0, 0, 0, 0, 0, 0, 0, 110],
                        expected_global_numeric_vector_length_after_event=None,
                        probe="Battle page executes 10050 opcode 40 records then a marker and Save01",
                        verification_status="inputs serialized; compare original and Player battle-event save")
    if args.save_variant == "end-command-battle":
        manifest.update(expected_save_slot=1, expected_global_slots_after_event=[99, 0, 0, 0, 0, 0, 0, 0, 0, 110],
                        expected_global_numeric_vector_length_after_event=None,
                        probe="Battle page writes marker, saves, and reaches the standard LCF opcode-0 list terminator without an explicit termination command",
                        verification_status="inputs serialized; capture original mode-2 cleanup and two following update calls")
    if args.save_variant == "end-nest-auto-common":
        manifest.update(expected_save_slot=1, expected_global_slots_after_event=[99, 77, 33],
                        expected_global_numeric_vector_length_after_event=None,
                        probe="Autostart parent branch scan and common-event 10050 End Nest records before parent save",
                        verification_status="inputs serialized; compare original and Player parent/common save")
    if args.save_variant == "opcode40-auto-common":
        manifest.update(expected_save_slot=1, expected_global_slots_after_event=[99, 77, 33],
                        expected_global_numeric_vector_length_after_event=None,
                        probe="Autostart parent branch scan and common-event 10050 opcode 40 records before parent save",
                        verification_status="inputs serialized; compare original and Player parent/common save")
    if args.save_variant.startswith("common1005-"):
        selected_ids = [1, 2, 1, 2, 1]
        for _ in range(14):
            selected_ids.extend((1, 2, 2, 2))
        selected_ids.append(1)
        snapshots = []
        checksum = 0
        for selected in selected_ids:
            checksum += selected
            snapshots.append(checksum)
        values = [0] * 161
        values[:4] = [106, 18, 44, 99]
        values[29:32] = [1, 30, -1]
        values[99:161] = snapshots
        if args.save_variant == "common1005-battle":
            values[9] = 110
        manifest.update(expected_save_slot=1, expected_global_slots_after_event=values,
                        expected_global_numeric_vector_length_after_event=None,
                        probe="62 opcode 1005 calls cover literal, single numeric and every low-nibble indirect mode; signed self/global reference chains, ignored high bits/Args/Text and per-call checksum snapshots",
                        verification_status="inputs serialized; compare original/Player common-frame IDs and checksum snapshots")
    if args.save_variant in {"parallel-self-save", "recursive-self-save"}:
        manifest["expected_global_slots_after_event"] = (
            [0, 0, 0, 42, 43, 42, 43] if args.save_variant == "parallel-self-save"
            else [0, 0, 0, 1, 42, 43])
        manifest["expected_global_numeric_vector_length_after_event"] = (
            8 if args.save_variant == "parallel-self-save" else 7)
        manifest["verification_status"] = "inputs serialized; compare original and Player self-bank save snapshots"
    if args.save_variant == "js-title-cycle":
        manifest["expected_global_slots_after_event"] = [0, 0, 0, 0, 42]
        manifest["expected_global_numeric_vector_length_after_event"] = 5
        manifest["verification_status"] = "inputs serialized; run probe-maniac-original-slot-zero.ps1 and verify the JavaScript title-cycle transitions"
    if args.save_variant == "title-cycle-save":
        manifest["expected_global_slots_after_event"] = [0, 0, 0, 0, 42]
        manifest["expected_global_numeric_vector_length_after_event"] = 6
        manifest["verification_status"] = "inputs serialized; observe two Save01/Save02 cycles across return to title and new game"
    if args.save_variant == "js-load-cycle":
        manifest["expected_global_slots_after_event"] = [0, 0, 0, 42, 43]
        manifest["expected_global_numeric_vector_length_after_event"] = 5
        manifest["verification_status"] = "inputs serialized; run probe-maniac-original-slot-zero.ps1 and verify JavaScript state after load"
    if args.save_variant in {"map-switch", "load-overwrite", "js-load-cycle", "legacy-controlvars", "legacy-negative-self", "string-self-source", "string-to-num-radix", "string-pop-line", "string-pop-line-range", "js-host-types", "js-host-accessors", "js-host-negative", "js-host-edges", "js-exception-continue", "js-host-inventory", "js-host-symbols", "reserved2-noop", "reserved-noop", "drawshape-noop", "noop-long-skip", "end-nest-long-skip", "end-nest-auto-common", "end-nest-battle", "opcode40-long-skip", "opcode40-auto-common", "opcode40-battle", "common1005-auto", "common1005-parallel", "common1005-nested", "common1005-battle", "end-command-battle", "noop-battle", "save-boundaries", "parallel-self-save", "recursive-self-save", "title-cycle-save"}:
        manifest["expected_save_slot"] = 1
    if args.save_variant == "js-host-negative":
        manifest.update(probe="Original 3030 getv/gets/gett and setv/sets/sett negative IDs in map and common-event contexts, with absent-bank and indirect negative ID reads",
                        expected_global_slots_after_event=[-1, 99],
                        expected_global_numeric_vector_length_after_event=2,
                        expected_global_switch_slots_after_event=[0],
                        expected_global_switch_vector_length_after_event=1)
    if args.save_variant == "js-host-edges":
        manifest.update(probe="Fixed original host scalar coercion, array/proxy access and conversion-error write order; JSON in string 1 and marker 99 in numeric 1",
                        js_edge_case=args.js_edge_case,
                        expected_global_slots_after_event=[99],
                        expected_global_numeric_vector_length_after_event=None,
                        verification_status="inputs serialized; capture original and verify JS edge results")
    if args.save_variant == "save-boundaries":
        manifest["expected_save_slot"] = 9
    if args.save_variant == "load-boundaries-seed":
        manifest.update(expected_save_slot=args.load_seed_slot,
                        load_seed_slot=args.load_seed_slot,
                        probe="Save a distinct slot marker, wait five seconds, then write continuation marker 99 and save slot 8")
        manifest["expected_global_slots_after_event"] = [args.load_seed_slot * 101]
        manifest["expected_global_numeric_vector_length_after_event"] = 1
    if args.save_variant == "load-boundaries":
        manifest.update(expected_save_slot=8, load_case=args.load_case,
                        probe="Skip ten empty/short/nonpositive/missing-slot 3003 records, save markers in slot 5, then load one of two original seeds and save the restored continuation in slot 8")
        manifest["expected_global_slots_after_event"] = [909 if args.load_case == 7 else 707] + [0] * 8 + [99]
        manifest["expected_global_numeric_vector_length_after_event"] = 10
    if args.save_variant in {"common1005-wait-auto", "common1005-wait-parallel", "common1005-wait-nested"}:
        manifest.update(expected_save_slot=1,
                        expected_global_slots_after_event=[10, 0, 3],
                        expected_global_numeric_vector_length_after_event=None,
                        probe=f"1005 {args.save_variant} child waits while parallel map event saves slot 1; fresh process loads it and parent saves slot 2 after continuation",
                        acceptance_id="W02-028",
                        verification_status="Fixed original/Player active-wait checkpoint and fresh-process continuation; other W02-028 contexts remain open")
    if args.save_variant in {"common1005-wait-battle", "call12330-wait-battle"}:
        opcode = 12330 if args.save_variant.startswith("call12330-") else 1005
        manifest.update(expected_save_slot=1, acceptance_id="W02-049" if opcode == 12330 else "W02-036",
                        expected_global_slots_after_event=[0, 0, 3, 0, 0, 0, 0, 0, 0, 109],
                        expected_global_numeric_vector_length_after_event=None,
                        probe=f"Battle {opcode} child saves slot 1 before five-second wait and slot 2 after; parent saves slot 4. Both native and Player LSD omit the live battle stack; loading slot 1 in a fresh process saves resumed map state to slot 3.",
                        verification_status="Battle execution, save boundary and fresh-process continuation are evaluated by verify-maniac-common1005-battle-wait.py")
    if args.save_variant in {"call12330-wait-auto", "call12330-wait-parallel", "call12330-wait-nested"}:
        context = args.save_variant.removeprefix("call12330-wait-")
        manifest.update(expected_save_slot=1,
                        expected_global_slots_after_event=[10, 0, 3],
                        expected_global_numeric_vector_length_after_event=None,
                        probe=f"12330 selector-0 {context} parent calls common child; parallel map event saves during child wait; fresh process resumes child and caller to Save02",
                        acceptance_id={"auto": "W02-046", "parallel": "W02-047", "nested": "W02-048"}[context],
                        verification_status="Checkpoint and fresh-process continuation require verify-maniac-common1005-wait.py --capture")
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
