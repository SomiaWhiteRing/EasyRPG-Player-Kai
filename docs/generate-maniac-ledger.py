"""Generate the Maniac audit ledger from the reviewed static catalog.

This creates audit candidates, not compatibility claims. Run from any directory:
    python docs/generate-maniac-ledger.py
"""

import csv
import hashlib
import json
import re
import runpy
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DOCS = ROOT / "docs"
REPORT = ROOT.parent / "RPGRewriter-maniac" / "analysis" / "Maniacs-Complete"
CONTRACTS = REPORT / "reviewed-contracts.json"
INVENTORY = DOCS / "maniac-250921-command-inventory.csv"
PLAN = DOCS / "maniac-completion-plan.md"
OUTPUT = DOCS / "maniac-submode-ledger.csv"
HELPERS_OUTPUT = DOCS / "maniac-shared-helper-references.csv"
HELPER_CONSUMERS_OUTPUT = DOCS / "maniac-shared-helper-consumers.csv"
ACCEPTANCE = DOCS / "maniac-acceptance-items.tsv"
RUNTIME = ROOT.parent / "RPGRewriter-maniac" / "patch_selfvar_250921" / "English" / "RPG_RT.exe"
STRINGS_3020_BRANCH_ACCEPTANCE = runpy.run_path(
    str(DOCS / "audit-maniac-3020-scope.py"))["BRANCH_ACCEPTANCE"]
GAME_INFO_3021_BRANCH_ACCEPTANCE = runpy.run_path(
    str(DOCS / "audit-maniac-3021-scope.py"))["BRANCH_ACCEPTANCE"]
REWRITE_MAP_3015_BRANCH_ACCEPTANCE = runpy.run_path(
    str(DOCS / "audit-maniac-3015-scope.py"))["BRANCH_ACCEPTANCE"]
DRAW_TILE_3028_BRANCH_ACCEPTANCE = runpy.run_path(
    str(DOCS / "audit-maniac-3028-scope.py"))["BRANCH_ACCEPTANCE"]
MESSAGE_HOOK_3029_BRANCH_ACCEPTANCE = runpy.run_path(
    str(DOCS / "audit-maniac-3029-scope.py"))["BRANCH_ACCEPTANCE"]
SELFVAR_3036_BRANCH_ACCEPTANCE = runpy.run_path(
    str(DOCS / "audit-maniac-3036-scope.py"))["BRANCH_ACCEPTANCE"]

HEADERS = [
    "ID", "opcode", "variant", "work_package", "reference_version_hash",
    "scenario_context", "platform", "parameters_and_modes", "short_record_rule",
    "text_role_and_length_unit", "read_write_objects", "lifecycle_and_wait",
    "save_fields", "player_implementation", "evidence", "gap_category",
    "dependencies", "acceptance_example", "acceptance_ids", "status", "actual_result", "blocker",
]

ACCEPTED_OPCODE = {
    "W01-001": 3036, "W01-002": 3036, "W01-003": 3036, "W01-004": 3036,
    "W02-001": 12330, "W02-002": 12310, "W02-003": 12310,
    "W02-004": 12330, "W03-001": 10220, "W04-004": 3020, "W04-005": 3020,
    "W04-006": 3020, "W04-007": 3020,
    "W11-008": 3002, "W11-009": 3002,
}

ARRAY_OPERATIONS = (
    "copy A2→A4", "swap", "sort ascending", "sort descending", "shuffle",
    "enumerate", "add", "subtract", "multiply", "divide", "remainder",
    "bitwise OR", "bitwise AND", "bitwise XOR", "shift left", "shift right logical",
    "dereference", "paired sort ascending", "paired sort descending", "paired shuffle",
    "fill", "reverse", "copy A4→A2",
)

CALL_COMMAND_MODES = (
    "array source", "single variable", "indirect array source",
    "inline values", "expression stream",
)

SELFVAR_MODES = {
    ("A0[3:0]", "target_environment"): ("global", "common event", "map event", "current frame", "caller frame", "unknown global fallback"),
    ("A0[19:16]", "source_environment"): ("global", "common event", "map event", "current frame", "caller frame", "unknown global fallback"),
    ("A0[7:4]", "target_common_id_mode"): ("literal ID", "numeric reference", "indirect numeric", "literal name", "string variable", "indirect string variable", "other raw ID"),
    ("A0[23:20]", "source_common_id_mode"): ("literal ID", "numeric reference", "indirect numeric", "literal name", "string variable", "indirect string variable", "other raw ID"),
    ("A0[11:8]", "target_map_id_mode"): ("literal", "numeric reference", "indirect numeric", "other literal mode", "Clear/Init negative-reference mode five"),
    ("A0[27:24]", "source_map_id_mode"): ("literal", "numeric reference", "indirect numeric", "other literal mode"),
    ("A0[15:12]", "target_slot_mode"): ("other: skip Set/Clear", "literal ID", "numeric reference"),
    ("A0[31:28]", "source_value_mode"): ("literal", "numeric reference", "indirect numeric", "other branch-specific literal"),
    ("A7[3:0]", "count_mode"): ("literal", "numeric reference", "indirect numeric or higher"),
    ("A7[7:4]", "value_kind"): ("number", "string", "switch", "all for Clear; numeric for Set", "other: no Clear write; numeric for Set"),
    ("A7[11:8]", "array_mode"): ("scalar", "array", "other: no Set assignment"),
}


def flatten(value):
    if value is None:
        return ""
    if isinstance(value, str):
        return value
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def helper_call(source: str, line_number: int, helper: str) -> tuple[str, list[str], str]:
    """Read one C++ call, including arguments that continue on later lines."""
    lines = source.splitlines(keepends=True)
    line = lines[line_number - 1]
    matches = list(re.finditer(r"\b" + re.escape(helper) + r"\b", line))
    if len(matches) != 1:
        raise ValueError(f"ambiguous helper token at {line_number}: {helper}")
    position = sum(map(len, lines[:line_number - 1])) + matches[0].start()
    cursor = position + len(helper)
    if source[cursor:cursor + 1] == "<":
        cursor = source.index(">", cursor) + 1
    while source[cursor:cursor + 1].isspace():
        cursor += 1
    if source[cursor:cursor + 1] != "(":
        raise ValueError(f"not a call at {line_number}: {helper}")
    cursor += 1
    depth = 1
    square = curly = 0
    quote = ""
    escaped = False
    arguments = []
    argument_start = cursor
    while depth:
        if cursor >= len(source):
            raise ValueError(f"unterminated helper call at {line_number}: {helper}")
        ch = source[cursor]
        if quote:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                arguments.append(source[argument_start:cursor].strip())
        elif ch == "[":
            square += 1
        elif ch == "]":
            square -= 1
        elif ch == "{":
            curly += 1
        elif ch == "}":
            curly -= 1
        elif ch == "," and depth == 1 and square == 0 and curly == 0:
            arguments.append(source[argument_start:cursor].strip())
            argument_start = cursor + 1
        cursor += 1
    prefix = line[:matches[0].start()].strip()
    invocation = " ".join(source[position:cursor].split())
    return invocation, [" ".join(arg.split()) for arg in arguments], prefix


def describe_helper_call(helper: str, args: list[str]) -> tuple[str, str, str, str]:
    """State the local selector, input, and rule without implying native parity."""
    if helper == "ValueOrVariable":
        if len(args) != 2:
            raise ValueError(f"ValueOrVariable argument count: {args}")
        return args[0], "", args[1], "0 literal; 1 numeric variable; 2 indirect numeric; 3 switch boolean; 4 indirect switch boolean; other -1 (patch and template gates apply)"
    if helper == "ValueOrVariableBitfield":
        if args[0] == "com":
            if len(args) != 4:
                raise ValueError(f"ValueOrVariableBitfield command arguments: {args}")
            mode, shift, value = f"com.parameters[{args[1]}]", args[2], f"com.parameters[{args[3]}]"
        else:
            if len(args) != 3:
                raise ValueError(f"ValueOrVariableBitfield arguments: {args}")
            mode, shift, value = args
        return mode, shift, value, "selected 4-bit nibble follows ValueOrVariable modes; command overload returns raw value when Maniac is off"
    if helper == "CommandStringOrVariable":
        if len(args) != 3:
            raise ValueError(f"CommandStringOrVariable arguments: {args}")
        return f"com.parameters[{args[1]}]", "", f"com.parameters[{args[2]}]", "0/other command Text; 1 string variable; 2 indirect string variable; short records fall back to Text"
    if helper == "CommandStringOrVariableBitfield":
        if len(args) != 4:
            raise ValueError(f"CommandStringOrVariableBitfield arguments: {args}")
        return f"com.parameters[{args[1]}]", args[2], f"com.parameters[{args[3]}]", "selected 4-bit nibble: 0/other command Text; 1 string variable; 2 indirect string variable; short records fall back to Text"
    if helper == "GetWithMode":
        if len(args) != 4:
            raise ValueError(f"GetWithMode arguments: {args}")
        return args[1], "", args[2], "1 global string variable; 2 indirect global string variable; other supplied Text data; this helper bypasses interpreter self-string context"
    if helper == "DecodeTargetEvaluationMode":
        if len(args) != 3:
            raise ValueError(f"DecodeTargetEvaluationMode arguments: {args}")
        return "com.parameters[0]", "", "com.parameters[1/2]", "0 single; 1 range; 2 indirect single; 3 indirect range if enabled; 4 expression if enabled; optional low-nibble mask and range reversal"
    raise ValueError(f"unknown helper {helper}")


def main():
    catalog = json.loads(CONTRACTS.read_text(encoding="utf-8"))
    inventory = list(csv.DictReader(INVENTORY.open(encoding="utf-8-sig", newline="")))
    by_opcode = {int(row["opcode"]): row for row in inventory}
    if len(inventory) != 168 or len(by_opcode) != 168:
        raise ValueError("inventory must contain 168 unique opcodes")
    plan_text = PLAN.read_text(encoding="utf-8")
    work = {int(opcode): package for opcode, package in re.findall(
        r"^\|\[(\d+)\]\([^\n]+?\)\|[^\n|]*\|(W\d\d)\|", plan_text, re.M
    )}
    if set(work) != set(by_opcode):
        raise ValueError("plan assignments and inventory differ")
    runtime_hash = hashlib.sha256(RUNTIME.read_bytes()).hexdigest()
    expected_runtime_hash = "082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f"
    if runtime_hash != expected_runtime_hash:
        raise ValueError("250921 runtime hash changed")

    # This ledger also holds reviewed rows entered after the catalog bootstrap.
    # Keep those results when refreshing catalog candidates and helper locations.
    reviewed = {}
    if OUTPUT.exists():
        with OUTPUT.open(encoding="utf-8-sig", newline="") as stream:
            for previous in csv.DictReader(stream):
                if (previous["status"] not in {"", "待审计", "实现中"}
                        and int(previous["opcode"]) not in {3006, 3015, 3028, 3029, 3038}):
                    previous["opcode"] = int(previous["opcode"])
                    reviewed[previous["ID"]] = previous

    rows = []
    for contract in sorted(catalog["commands"], key=lambda item: item["opcode"]):
        opcode = contract["opcode"]
        inv = by_opcode[opcode]
        evidence = ";".join(contract.get("evidence", []))
        category = "待核实"
        if opcode in {3022, 3023, 3024, 3034, 3035, 40}:
            category = "原版空操作"
        elif opcode in {3027, 3031}:
            category = "父命令消费"
        elif opcode in {3025, 3026, 3028, 3029, 3030, 3032, 3033, 3036, 3037, 3038}:
            category = "缺失"
        elif 3001 <= opcode <= 3021:
            category = "部分实现"

        base = {
            "opcode": opcode,
            "work_package": work[opcode],
            "reference_version_hash": f"250921:{runtime_hash}",
            "scenario_context": "待按地图/公共事件/战斗/并行上下文审计",
            "platform": "Windows",
            "short_record_rule": "原始参数长度保留；缺失数值读取为 0；逐分支核对",
            "text_role_and_length_unit": flatten(contract.get("text")),
            "read_write_objects": "待从运行时消费者提取",
            "lifecycle_and_wait": "待审计",
            "save_fields": "待审计",
            "player_implementation": inv["player_handler"] or inv["player_dispatch"],
            "evidence": evidence,
            "gap_category": category,
            "dependencies": work[opcode],
            "acceptance_example": "待定义原版与 Player 同初态对照样例",
            "acceptance_ids": ";".join(key for key, accepted_opcode in ACCEPTED_OPCODE.items() if accepted_opcode == opcode),
            "status": "待审计",
            "actual_result": "",
            "blocker": "",
        }
        if opcode in {10210, 10220, 12310, 12330, 3036}:
            base["status"] = "实现中"
            base["gap_category"] = "部分实现"
            base["actual_result"] = "Player 局部分支有专项测试；逐模式与 Windows 原版对照未完成"
        if opcode == 3036 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="missing A0..A9 zero; writer trims trailing zeros; retain short/extra Args, flags, lengths and Text",
                text_role_and_length_unit='{"role":"ordered_common_names_and_literal_string","length_unit":"writer ANSI bytes; runtime approximate UTF16-unit cost"}',
                read_write_objects="five numeric/switch/string environments; target/source allocation; command name cache and shared Text cursor; arrays and refresh",
                lifecycle_and_wait="Set allocates target/source before gates; Clear/Init existing only; self Init frees vectors, global retains capacity; config map refresh, no own wait",
                save_fields="global/self vector state and interpreter continuation; runtime-only command cache; remaining context/save proof W01-030",
                evidence=evidence + ";docs/audit-maniac-3036-scope.py;docs/evidence/maniac-250921-3036-scope-audit.json",
                gap_category="scope audited; partial Selfvar differential evidence retained",
                dependencies="C250921-03036;W01-006;W01-008;W01-009",
                acceptance_example="W01-001..004 narrow accepted samples; 018..022 environments; 023..025 names; 026..029 arrays/defects; 030 records/context",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed original Set/Clear/Init branch scope; Player dispatcher exists, full functional parity remains open",
                blocker="-",
            )
        elif opcode == 3015 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="A0..A8 absent numeric words zero; native writer emits nine words without Trim; preserve count, high bits, tail and Text",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text read"}',
                read_write_objects="current map lower/upper uint16 tile storage; global/current-self numeric range source; lower autotile neighbor state",
                lifecycle_and_wait="X/Y/W/H before source, early return on nonpositive size; direct success, no opcode-owned wait or explicit event refresh",
                save_fields="map tile storage, rendering and persistence require W05-015 dynamic acceptance; no command-owned save field",
                evidence=evidence + ";docs/audit-maniac-3015-scope.py;docs/evidence/maniac-250921-3015-scope-audit.json",
                gap_category="scope audited; Player lacks variable-range source and original direct-write/neighbor parity",
                dependencies="C250921-03015;W05-010",
                acceptance_example="W05-011..015 original/Player modes, source, direct-write defect, autotile and continuation",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed 3015 single/range, lower/upper, autotile/direct and map-storage static scope; dynamic parity remains open",
                blocker="-",
            )
        elif opcode == 3013:
            if inv.get("effective_branch_ids"):
                base.update(
                    scenario_context="shared dispatcher: map autostart/parallel, called common, battle; 3019-generated candidate",
                    short_record_rule="original parameter length retained; missing A0..A4 reads zero; high A1 bits, extra Args and Text preserved",
                    read_write_objects="global/current-self numeric arrays, shared RNG, map event refresh; helper allocation includes extra slot",
                    lifecycle_and_wait="resolve base/count before dispatch; driver-dependent per-map-event refresh except op16/21; no independent wait",
                    save_fields="global/self arrays, RNG and map event state require target-specific save/restart acceptance",
                    evidence=evidence + ";docs/audit-maniac-3013-scope.py;docs/evidence/maniac-250921-3013-scope-audit.json",
                    gap_category="scope audited; 23 operation and shared feature groups open",
                    dependencies="C250921-03013;W01-009",
                    acceptance_example="See maniac-250921-3013-scope-audit.json for stable operation/branch destinations",
                    status="范围已拆解",
                    actual_result="Fixed original static consumer contract and helper bytes audited; existing Player operation tests remain partial and do not close full original/Player parity",
                )
            else:
                base["status"] = "实现中"
                base["actual_result"] = "Player 424 项测试通过；3013 专项覆盖部分分支和即时页面刷新，原版运行对照未执行"
                base["acceptance_example"] = "tests/game_interpreter.cpp:Maniac variable arrays；与 Windows 250921 同初态对照"
        elif opcode == 3022 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="no Args/Text reads or runtime minimum count; retain all decoded words, Text and indent",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text length read"}',
                read_write_objects="configuration driver byte 26944a; shared frame depth and advanced cursor; no expression value or storage mutation",
                lifecycle_and_wait="driver zero returns success; nonzero refetches next record inside dispatcher invocation; no independent wait",
                save_fields="record and frame/cursor retention; no opcode-owned persistent state; timing/restart acceptance W02-031",
                evidence=evidence + ";docs/audit-maniac-3022-scope.py;docs/evidence/maniac-250921-3022-scope-audit.json",
                dependencies="C250921-03022;W02-008",
                acceptance_ids="W02-031",
                status="范围已拆解",
                actual_result="Fixed original has two pf configuration-dependent scheduling branches, neither evaluates Args; original/Player context timing remains W02-031",
                acceptance_example="W02-031: both driver values in map autostart/parallel, common and battle; short/extended Args and Text retained; evaluator side effects absent",
            )
        elif opcode == 3023 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="runtime ignores all Args; no minimum count; retain original short/long/malformed words",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text length read; formatter-only counted byte segments"}',
                read_write_objects="shared advanced command cursor and success return only; no destination/reference lookup, assignment or Text consumption",
                lifecycle_and_wait="unconditional success return; no independent wait; original/Player continuation and budget parity remain W02-032",
                save_fields="record and frame/cursor retained; no opcode-owned persistent state; save/restart continuation acceptance W02-032",
                evidence=evidence + ";docs/audit-maniac-3023-scope.py;docs/evidence/maniac-250921-3023-scope-audit.json",
                dependencies="C250921-03023;W02-008",
                acceptance_ids="W02-032",
                status="范围已拆解",
                actual_result="PE opcode-selector byte 47ebb chooses slot 29 at 47dc8 and success 47d01; formatter destination modes are not runtime modes; W02-032 unvalidated",
                acceptance_example="W02-032: four shared contexts; marker and cursor timing; zero/short/long/malformed Args and arbitrary Text retained with no value effects",
            )
        elif opcode == 3004 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="runtime ignores all Args; no minimum count; retain original short/long/malformed words, Text and indent",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text length read"}',
                read_write_objects="27916a end-load byte; conditional type-12 operand-0 reads it; first map update reads 279168/27916a/27916b",
                lifecycle_and_wait="immediate success after setting flag; next map update may bypass two-count startup delay and call brightness/presentation helper",
                save_fields="flag reset boundary, map update timing and save/restart persistence require W11-027/028 validation",
                evidence=evidence + ";docs/audit-maniac-3004-scope.py;docs/evidence/maniac-250921-3004-scope-audit.json",
                gap_category="scope audited; Player flag and map behavior open",
                dependencies="C250921-03004;W11-006",
                acceptance_ids="W11-027;W11-028",
                status="范围已拆解",
                actual_result="Fixed original branch and direct conditional/map consumers audited; Player handler still returns success without flag or map behavior; dynamic parity remains open",
                acceptance_example="W11-027 conditional before/after/repeat and retained payload; W11-028 next-update delay/brightness/reset/save trajectory",
            )
        elif opcode == 3005 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="A0/A1 missing read as zero; fewer than four Args use scaled client coordinates; A3 present only when count >=4; retain extra Args",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text length read"}',
                read_write_objects="OS cursor and client window; scaling factors 2695c0/c8; global/current-self numeric destinations; current-map event refresh gated by pf 26944a",
                lifecycle_and_wait="write X then Y; pf zero refreshes each current-map event and returns success, nonzero skips refresh and refetches within shared invocation; no independent wait",
                save_fields="global/self numeric banks and interpreter cursor; restart continuation W10-005",
                evidence=evidence + ";docs/audit-maniac-3005-scope.py;docs/evidence/maniac-250921-3005-scope-audit.json",
                gap_category="scope audited; Player coordinate/signed destination/pf behavior open",
                dependencies="C250921-03005;W10-001",
                acceptance_ids="W10-004;W10-005",
                status="范围已拆解",
                actual_result="Original 3005 consumer and helpers statically audited; Player global-only/unconditional-refresh handler is not accepted for full parity",
                acceptance_example="W10-004 raw/scaled coordinates, signed/zero and short/extra payload; W10-005 pf refresh and four-context scheduling",
            )
        elif opcode == 3006 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="missing A0/A1/A2 reads zero; A3 read only when count >=4; retain extra Args",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text length read"}',
                read_write_objects="global/current-self numeric source, game coordinate bounds, scaling factors, client/screen cursor; no variable assignment",
                lifecycle_and_wait="ClientToScreen then SetCursorPos; pf zero success, nonzero same-invocation refetch; no own wait or event refresh",
                save_fields="original record and shared frame/cursor; restart continuation W10-008",
                evidence=evidence + ";docs/audit-maniac-3006-scope.py;docs/evidence/maniac-250921-3006-scope-audit.json",
				gap_category="scope audited; Player SetMousePosition implementation awaits cursor comparison and 3038 zoom",
                dependencies="C250921-03006;W10-001",
                acceptance_ids="W10-007;W10-008",
                status="范围已拆解",
				actual_result="Original 3006 consumer statically audited; Player Windows handler implemented with zoom fixed at one; dynamic parity open",
                acceptance_example="W10-007 source/clamp/raw/OS cursor parity; W10-008 pf and context scheduling",
            )
        elif opcode == 3007 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                read_write_objects="Picture ID and packed A0/A13/A23 references; generated UTF-16 body/system/font, picture fields, pixel backing and shared renderer",
                lifecycle_and_wait="immediate picture image replacement; no opcode-owned wait; shared pf zero success/nonzero refetch",
                save_fields="picture state and original Args/Text retention; saved pixel/runtime reconstruction still open",
                gap_category="static scope audited; Player and original dynamic comparison open",
                dependencies="C250921-03007;W06-001",
                acceptance_example="W06-004 fields; W06-005 Text/glyph output; W06-006 lifecycle/pf/context",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed original 3007 consumer and linked text contract; feature parity remains open",
                blocker="-",
            )
        elif opcode == 3008 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="Missing consumed words default zero; picture validated before type; geometry uses A0-A7, pixel uses A0-A8; retain original count and extra words",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text length read"}',
                read_write_objects="picture fields/buffers and packed numeric references; geometry signed global/self destinations; pixel contiguous global/self storage",
                lifecycle_and_wait="no opcode-owned wait; geometry only pf-zero event refresh; shared pf zero success/nonzero refetch",
                save_fields="picture/numeric banks and shared cursor; pending-image and restart continuation W06-010",
                evidence=evidence + ";docs/audit-maniac-3008-scope.py;docs/evidence/maniac-250921-3008-scope-audit.json",
                gap_category="scope audited; Player geometry/reference/pixel/scheduling parity open",
                dependencies="C250921-03008;W06-001",
                acceptance_example="W06-008 geometry/type/record; W06-009 pixel/buffer/output; W06-010 refresh/pf/context",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed original geometry and pixel consumers audited; Player feature and dynamic parity remain open",
                blocker="-",
            )
        elif opcode == 3038 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="A0..A3 missing words read zero; no four-word minimum; retain original Args count and extra words",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text length read"}',
                read_write_objects="packed A0 operation/type/value sources; screen and audio option state, signed global/self get destination; type1 zoom feeds 3006 raw coordinate limit",
                lifecycle_and_wait="get/set option side effects; direct success exit, no opcode-owned wait or pf refetch",
                save_fields="option current/default state and frame/cursor; persistence and restart behavior remain W08 feature acceptance",
                player_implementation="missing 3038 dispatcher and handler",
                evidence=evidence + ";docs/audit-maniac-3038-scope.py;docs/evidence/maniac-250921-3038-scope-audit.json",
                gap_category="scope audited; Player media option command absent",
				dependencies="C250921-03038;W08-004",
                acceptance_example="W08-005 zoom/3006; W08-006 visual settings; W08-007 audio volume; W08-008 operands, contexts and continuation",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed original 3038 get/set options and zoom-to-3006 link audited; Player has no 3038 dispatch; dynamic parity remains open",
                blocker="-",
            )
        elif opcode == 3018 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="count 0/1 clears inactive; option0 missing A2 clears it; other consumed A2/A3 missing words zero; preserve original length and extra Args",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text length read"}',
                read_write_objects="whole/nibble A0 signed global/self sources; inactive, FPS, picture/animation limits, frame skip, mouse, battle alignment, face state",
                lifecycle_and_wait="consumer side effects; shared pf zero success/nonzero refetch; no opcode-owned wait/refresh",
                save_fields="option state and dependent resource/frame state; persistence and restart behavior remain W08 feature acceptance",
                evidence=evidence + ";docs/audit-maniac-3018-scope.py;docs/evidence/maniac-250921-3018-scope-audit.json",
                gap_category="scope audited; Player handles only picture-limit as noop; remaining option effects absent",
                dependencies="C250921-03018;W08-001",
                acceptance_example="W08-009 timing; W08-010 resources; W08-011 UI/battle; W08-012 operands and continuation",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed 3018 options 0..7 and unknown fallback statically audited; Player dynamic parity remains open",
                blocker="-",
            )
        elif opcode == 3017 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="A0..A5 missing words default zero; count<=0 or same start returns before range checks; retain original count and extra Args",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text length read"}',
                read_write_objects="A1 source/count/target modes; picture pointer slots and object IDs; 6c4f0 image/name/effect erase helper",
                lifecycle_and_wait="ordered move/swap/slide; shared pf zero success/nonzero refetch; no opcode-owned wait/refresh",
                save_fields="picture slot/object identity and active image state; save/restart acceptance W06-013 and W11-005",
                evidence=evidence + ";docs/audit-maniac-3017-scope.py;docs/evidence/maniac-250921-3017-scope-audit.json",
                gap_category="scope audited; Player ChangePictureId remains warning stub",
                dependencies="C250921-03017;W06-001",
                acceptance_example="W06-011 ordered slot moves; W06-012 invalid/erase; W06-013 modes/context/continuation",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed 3017 move/swap/slide and erasure scope audited; Player dynamic parity remains open",
                blocker="-",
            )
        elif opcode == 3029 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="missing A0..A6 zero; writer emits seven untrimmed words; preserve count, high flags, extra Args and Text",
                text_role_and_length_unit='{"role":"mode3_common_event_identifier_else_preserve","length_unit":"runtime full UTF16 name, writer stores ANSI byte count A2"}',
                read_write_objects="common event selector/cache, four signed global/self target bases, four hook packets and readback, message state and dedicated event stack",
                lifecycle_and_wait="hook event runs under current budget/wait gate on temporary stack; caller stack restored before readback; opcode itself direct success",
                save_fields="hook configuration, message/interpreter state and saved continuation; W06-024 remains open",
                player_implementation="missing 3029 dispatcher and handler",
                evidence=evidence + ";docs/audit-maniac-3029-scope.py;docs/evidence/maniac-250921-3029-scope-audit.json",
                gap_category="scope audited; Player Control Message hook absent",
                dependencies="C250921-03029;W06-002;W02-008",
                acceptance_example="W06-019 selector/bases; 020 user; 021 create; 022 destroy; 023 blit; 024 stack/continuation",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed 3029 selector, four hook packets, readback and temporary-stack static scope; no Player parity accepted",
                blocker="-",
            )
        elif opcode == 3028 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="missing A0..A11 zero; writer emits12 words but creation default is7; preserve original count, unused Text, high option bits and tail",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text read"}',
                read_write_objects="signed numeric source range, zeroed temporary tile rectangle, current/temporary tileset, detached picture backing and dirty/resource state",
                lifecycle_and_wait="source resolves before size guard; direct success; no opcode-owned wait or pf refetch; clear before sheet-cell offsets",
                save_fields="mutated picture pixels and identity, resource lifetime, saved continuation; W06-018 remains open",
                player_implementation="missing 3028 dispatcher and handler",
                evidence=evidence + ";docs/audit-maniac-3028-scope.py;docs/evidence/maniac-250921-3028-scope-audit.json",
                gap_category="scope audited; Player Draw Tile On Picture absent",
                dependencies="C250921-03028;W06-001",
                acceptance_example="W06-014 operands; W06-015 tile buffer; W06-016 tileset/phase; W06-017 pixels; W06-018 lifecycle/context",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed 3028 source/layer/autotile/image and animation branch scope; no Player parity accepted",
                blocker="-",
            )
        elif opcode == 3021 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="missing A0..A7 words zero; zero/one-word records act as operation0; preserve count/tail/unknown flags and Text",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text read"}',
                read_write_objects="map/tile/window/pixel/frame/tileset/face/graphic/camera/shake/BGM sources; global/current-self numeric/string destinations",
                lifecycle_and_wait="branch outputs and shared pf gate; no opcode-owned wait; interpreter nest accesses live frames",
                save_fields="output banks and current map/image/frame/audio state; save/restart acceptance remains W05-004..009",
                evidence=evidence + ";docs/audit-maniac-3021-scope.py;docs/evidence/maniac-250921-3021-scope-audit.json",
                gap_category="scope audited; Player pixel/interpreter absent and other output/mode parity open",
                dependencies="C250921-03021;W05-003;W02-008;W06-001",
                acceptance_example="W05-004..009 original/Player comparison; W05-010 retains 3015 separately",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed 3021 operation, mode, source and consumer static scope; full original/Player parity remains open",
                blocker="-",
            )
        elif opcode == 3020 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="missing consumed A0..A7 zero; low32 arg count; no bundled writer/default Args; preserve original count, tail and Text",
                read_write_objects="signed global/current-self string ranges; numeric/switch outputs; Text/packed or filesystem files, regex captures",
                lifecycle_and_wait="target preallocation before operation validation; sequential operations and shared pf gate; no opcode-owned wait",
                save_fields="string/numeric/switch banks, files and frame/cursor; existing string save is W01-005, remaining feature/restart parity open",
                evidence=evidence + ";docs/audit-maniac-3020-scope.py;docs/evidence/maniac-250921-3020-scope-audit.json",
                gap_category="scope audited; Player string operations, source variants, encoding and self-bank parity open",
                dependencies="C250921-03020;W04-001;W01-007",
                acceptance_example="W04-008..016 original/Player comparisons; W04-004..007 are narrow closed fixtures only",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed 3020 operation/source/target/file/regex static scope; full original/Player parity remains open",
                blocker="-",
            )
        elif opcode == 3016 and inv.get("effective_branch_ids"):
            base.update(
                scenario_context=inv["applicable_contexts"],
                short_record_rule="empty record opens; absent A0..A5 zero; native writer emits one word for operations<=3, six for copy; retain original length/tail",
                text_role_and_length_unit='{"role":"runtime_unused_preserve","length_unit":"no runtime Text length read"}',
                read_write_objects="A1 nibble modes; local and shared switch/numeric/string banks; Save.lgs shared cache, flags and file blocks",
                lifecycle_and_wait="open/close/save/save-close or copy; shared pf zero success/nonzero refetch; file guard can prompt; no opcode-owned wait/refresh",
                save_fields="shared switch/numeric/string cache and Save.lgs; ordinary LSD is separate W11-006",
                evidence=evidence + ";docs/audit-maniac-3016-scope.py;docs/evidence/maniac-250921-3016-scope-audit.json",
                gap_category="scope audited; Player shared-save lifecycle, string copy and file parity open",
                dependencies="C250921-03016;W11-029",
                acceptance_example="W11-030 lifecycle; W11-031 switch/numeric; W11-032 strings; W11-033 file; W11-034 operands/context",
                acceptance_ids=inv["acceptance_ids"],
                status="范围已拆解",
                actual_result="Fixed 3016 operations and Save.lgs/cache consumers audited; Player dynamic parity remains open",
                blocker="-",
            )
        elif opcode == 3019:
            if inv.get("effective_branch_ids"):
                base.update(
                    scenario_context=inv["applicable_contexts"],
                    read_write_objects="generated opcode/Params/Text; global/current-self numeric banks; captured string; shared synthetic buffer; original owner/frame/cursor",
                    lifecycle_and_wait="same-invocation redispatch; generated 3019 skips before Text/Args; busy target may rewind original cursor and regenerate",
                    save_fields="original caller frame/cursor/banks and generated target effects; retry/restore acceptance W03-009",
                    evidence=evidence + ";docs/audit-maniac-3019-scope.py;docs/evidence/maniac-250921-3019-scope-audit.json",
                    dependencies="C250921-03019;W02-008",
                    acceptance_ids=inv["acceptance_ids"],
                    gap_category="scope audited; functional groups remain",
                    status="范围已拆解",
                    actual_result="W02-029/030 provide four-context original/Player 3019-to-1005 partial evidence; opcode/Text/array/inline/expression/redispatch groups remain W03-005..009",
                    acceptance_example="Each effective branch is mapped in maniac-250921-3019-scope-audit.json; fixed 1005 examples do not close all target or boundary semantics",
                )
            else:
                base["status"] = "实现中"
                base["actual_result"] = "Player 同帧派发与部分参数来源专项通过；原版运行对照未执行"
                base["acceptance_example"] = "tests/game_interpreter.cpp:Maniac CallCommand；与 Windows 250921 同初态对照"
        branches = []
        for field in contract.get("fields", []):
            values = field.get("values")
            if not isinstance(values, dict):
                continue
            for mode, meaning in values.items():
                branches.append((field, mode, meaning))

        if opcode == 3013:
            branches.extend(({"arg": 0, "name": "operation"}, str(mode), meaning)
                            for mode, meaning in enumerate(ARRAY_OPERATIONS))
        elif opcode == 3019:
            branches.extend(({"arg": "A0[7:4]", "name": "argument_source"}, str(mode), meaning)
                            for mode, meaning in enumerate(CALL_COMMAND_MODES))
        elif opcode == 3036:
            for (arg, name), meanings in SELFVAR_MODES.items():
                for mode, meaning in enumerate(meanings):
                    if name == "target_map_id_mode" and mode == 4:
                        mode_key = "5"
                    elif meaning.startswith(("other", "unknown")):
                        mode_key = "other"
                    else:
                        mode_key = str(mode)
                    branches.append(({"arg": arg, "name": name}, mode_key, meaning))

        # A command row tracks consumers and fields without an explicit mode table.
        row = base | {
            "ID": f"{opcode}:command",
            "variant": "命令整体/未列举的运行分支",
            "parameters_and_modes": ";".join(
                f"{field.get('arg', field.get('from_arg', '?'))}:{field.get('name', '?')}"
                for field in contract.get("fields", [])
            ),
        }
        if opcode == 3013 and inv.get("effective_branch_ids"):
            row["parameters_and_modes"] = "A0 operation 0..22; A1 base/count/operand modes; A2 signed base; A3 count; A4 array/numeric operand"
            row["acceptance_ids"] = inv["acceptance_ids"]
        elif opcode == 3022 and inv.get("effective_branch_ids"):
            row["parameters_and_modes"] = inv["effective_branch_ids"]
        elif opcode == 3023 and inv.get("effective_branch_ids"):
            row["parameters_and_modes"] = inv["effective_branch_ids"] + "; formatter-only fields are retained data"
        elif opcode == 3004 and inv.get("effective_branch_ids"):
            row["variant"] = "End Load Process flag and direct consumers"
            row["parameters_and_modes"] = "No opcode-owned Args/Text modes; sets 27916a=1"
        elif opcode == 3005 and inv.get("effective_branch_ids"):
            row["variant"] = "3005 coordinate/write/refresh branch scope"
            row["parameters_and_modes"] = "A0 signed X destination; A1 signed Y destination; A2 ignored; A3 bit0 raw-client bypass; pf config refresh/scheduling"
        elif opcode == 3006 and inv.get("effective_branch_ids"):
            row["variant"] = "3006 source, coordinate and scheduling branch scope"
            row["parameters_and_modes"] = "A0=0 literal A1/A2; any nonzero A0 one-lookup signed global/self A1/A2; A3 bit0 raw-client path"
        elif opcode == 3038 and inv.get("effective_branch_ids"):
            row["variant"] = "3038 get/set option, packed operand and direct-success scope"
            row["parameters_and_modes"] = "A0 nibble modes resolve operation A1, type A2, value/destination A3; get/set types 0..9 plus inactive boundaries"
        elif opcode == 3018 and inv.get("effective_branch_ids"):
            row["variant"] = "3018 options and unknown-option fallback scope"
            row["parameters_and_modes"] = "A1 selects option 0..7 or default frame-skip branch; A0 whole/nibble source and A2/A3 depend on option"
        elif opcode == 3017 and inv.get("effective_branch_ids"):
            row["variant"] = "3017 picture slot transform, error and erase scope"
            row["parameters_and_modes"] = "A0 move/swap/slide; A1 source/count/target modes; A2 source, A3 count, A4 target/offset, A5 ignore errors"
        elif opcode == 3015 and inv.get("effective_branch_ids"):
            row["variant"] = "3015 map tile source, layer, bounds and autotile scope"
            row["parameters_and_modes"] = "A0 nibbles0..4 select source/X/Y/W/H modes; A1/A2 bit0 select range/layer; A3..A7 source/rectangle; A8 disables lower autotile"
        elif opcode == 3028 and inv.get("effective_branch_ids"):
            row["variant"] = "3028 temporary tile rectangle and picture draw scope"
            row["parameters_and_modes"] = "A0 eight modes, A1 picture, A2/A3 pixel XY, A4/A5 tile WH, A6 source, A7 options, A8 range, A9 layer, A10 tileset, A11 time"
        elif opcode == 3029 and inv.get("effective_branch_ids"):
            row["variant"] = "3029 common event selector, hook packets and message stack scope"
            row["parameters_and_modes"] = "A0 event selector, A1 hook mask, A2 ID/name value, A3/A4 system and A5/A6 user signed destination bases"
        elif opcode == 3036 and inv.get("effective_branch_ids"):
            row["variant"] = "3036 five environments, three bank kinds, operations, names and arrays"
            row["parameters_and_modes"] = "A0 eight modes; A1/A2 target environment/map; A3 slot; A4/A5 source environment/map; A6 value; A7 count/kind/array; A8 count; A9 operation"
        elif opcode == 3021 and inv.get("effective_branch_ids"):
            row["variant"] = "3021 game information operation, source and destination scope"
            row["parameters_and_modes"] = "A0 field-specific numeric modes; A1 whole operation; A2..A7 branch operands/destinations"
        elif opcode == 3020 and inv.get("effective_branch_ids"):
            row["variant"] = "3020 target, operation, source and auxiliary consumer scope"
            row["parameters_and_modes"] = "A0 signed target/operand modes; A3 operation/source/flags; A4..A7 field-specific inputs and outputs"
        elif opcode == 3016 and inv.get("effective_branch_ids"):
            row["variant"] = "3016 shared-save lifecycle, copy and file scope"
            row["parameters_and_modes"] = "A0 open/close/save/save-close/copy directions; A1 packed modes; A2 type; A3 local, A4 shared, A5 count"
        if opcode in {10220, 12310, 12330} or (opcode == 3036 and not inv.get("effective_branch_ids")):
            row["acceptance_example"] = (
                "docs/generate-maniac-original-fixture.py:当前地图事件槽0、自变量、"
                "公共事件 selector 0/3/4/5/6/7 与数值及开关直接/间接返回固定序列；"
                "其余有效模式逐项对照"
            )
            row["actual_result"] = (
                "Windows 250921 原版与 Player 固定工程差分：全局数值槽0–12="
                "7,7,42,42,99,0,99,0,0,3,2,3,2；"
                "开关物理槽0–2=0,2,3（写入/复制/清除槽0）；"
                "10220 全局目标额外预扩容到原版物理长度14；"
                "见 build/maniac-original-slot-zero-trace.json "
                "及 tests/game_interpreter.cpp 的原版工程夹具。其它模式未验收"
            ) if opcode in {10220, 3036} else (
                "Windows 250921 原版与 Player 公共事件调用差分：数值/开关实参分别写入被调 bank；"
                "数值返回全局槽13=77及调用者self槽3=88；开关原始值3返回后规范化为1，"
                "直接写全局槽3/调用者self槽2，间接写全局槽4/调用者self槽5；"
                "正间接地址在返回时读取变化后的全局数值槽20=4；"
                "selector 3/4/5/6/7 分别经变量、二次间接、字面名称、字符串变量和间接字符串变量"
                "选到公共事件1，依次返回66–70；三类偏移来源均将公共事件1解析到事件2，"
                "开关原始值3返回到数值目标仍为3。selector 1/2和其它返回类型未验收"
            )
        rows.append(row)
        if opcode == 3038 and inv.get("effective_branch_ids"):
            for branch in inv["effective_branch_ids"].split(";"):
                if branch == "3038:other-operation-or-option-noop":
                    acceptance_ids = "W08-008"
                else:
                    option = int(branch.split("-")[2])
                    acceptance_ids = (
                        "W08-005;W08-008" if option == 1 else
                        "W08-007;W08-008" if option == 9 else "W08-006;W08-008")
                rows.append(base | {
                    "ID": branch,
                    "variant": branch,
                    "parameters_and_modes": "Resolved operation/type and signed A3 value/destination; see fixed 3038 scope audit",
                    "acceptance_ids": acceptance_ids,
                })
        if opcode == 3018 and inv.get("effective_branch_ids"):
            acceptance_by_branch = {
                "3018:short-count-inactive-clear": "W08-009;W08-012",
                "3018:option-0-inactive": "W08-009;W08-012",
                "3018:option-1-fps": "W08-009;W08-012",
                "3018:option-2-picture-limit": "W08-010;W08-012",
                "3018:option-3-frame-skip": "W08-009;W08-012",
                "3018:option-4-message-mouse": "W08-011;W08-012",
                "3018:option-5-battle-alignment": "W08-011;W08-012",
                "3018:option-6-animation-buffers": "W08-010;W08-012",
                "3018:option-7-face-cell": "W08-011;W08-012",
                "3018:unknown-option-frame-skip": "W08-009;W08-012",
            }
            for branch in inv["effective_branch_ids"].split(";"):
                rows.append(base | {
                    "ID": branch,
                    "variant": branch,
                    "parameters_and_modes": "Option-specific source and state; see fixed 3018 scope audit",
                    "acceptance_ids": acceptance_by_branch[branch],
                })
        if opcode == 3017 and inv.get("effective_branch_ids"):
            for branch in inv["effective_branch_ids"].split(";"):
                acceptance_ids = (
                    "W06-012;W06-013" if branch == "3017:invalid-source-or-target" else
                    "W06-011;W06-012" if branch == "3017:erase-and-picture-object-id" else
                    "W06-011;W06-013")
                rows.append(base | {
                    "ID": branch,
                    "variant": branch,
                    "parameters_and_modes": "Resolved range and picture slots; see fixed 3017 scope audit",
                    "acceptance_ids": acceptance_ids,
                })
        if opcode == 3036 and inv.get("effective_branch_ids"):
            for branch in inv["effective_branch_ids"].split(";"):
                rows.append(base | {
                    "ID": branch,
                    "variant": branch,
                    "parameters_and_modes": "See fixed 3036 operation, environment, scalar/array, Text/cache and defect rules in scope audit",
                    "acceptance_ids": SELFVAR_3036_BRANCH_ACCEPTANCE[branch],
                })
        if opcode == 3029 and inv.get("effective_branch_ids"):
            for branch in inv["effective_branch_ids"].split(";"):
                rows.append(base | {
                    "ID": branch,
                    "variant": branch,
                    "parameters_and_modes": "See fixed 3029 selector, packet, signed-bank and temporary-stack rules in scope audit",
                    "acceptance_ids": MESSAGE_HOOK_3029_BRANCH_ACCEPTANCE[branch],
                })
        if opcode == 3028 and inv.get("effective_branch_ids"):
            for branch in inv["effective_branch_ids"].split(";"):
                rows.append(base | {
                    "ID": branch,
                    "variant": branch,
                    "parameters_and_modes": "See fixed 3028 operand, temporary buffer, tileset/pattern and picture mutation rules in scope audit",
                    "acceptance_ids": DRAW_TILE_3028_BRANCH_ACCEPTANCE[branch],
                })
        if opcode == 3015 and inv.get("effective_branch_ids"):
            for branch in inv["effective_branch_ids"].split(";"):
                rows.append(base | {
                    "ID": branch,
                    "variant": branch,
                    "parameters_and_modes": "See fixed 3015 operand, source, direct-write, autotile and record rule in scope audit",
                    "acceptance_ids": REWRITE_MAP_3015_BRANCH_ACCEPTANCE[branch],
                })
        if opcode == 3021 and inv.get("effective_branch_ids"):
            for branch in inv["effective_branch_ids"].split(";"):
                rows.append(base | {
                    "ID": branch,
                    "variant": branch,
                    "parameters_and_modes": "See fixed 3021 operation, field mode, destination and record rule in scope audit",
                    "acceptance_ids": GAME_INFO_3021_BRANCH_ACCEPTANCE[branch],
                })
        if opcode == 3020 and inv.get("effective_branch_ids"):
            for branch in inv["effective_branch_ids"].split(";"):
                rows.append(base | {
                    "ID": branch,
                    "variant": branch,
                    "parameters_and_modes": "See fixed 3020 target/operation/source rule and field-specific modes in scope audit",
                    "acceptance_ids": STRINGS_3020_BRANCH_ACCEPTANCE[branch],
                })
        if opcode == 3016 and inv.get("effective_branch_ids"):
            acceptance_by_branch = {
                "3016:open-or-empty-lazy-load": "W11-030;W11-033;W11-034",
                "3016:close-discard": "W11-030;W11-034",
                "3016:save-if-open": "W11-030;W11-033;W11-034",
                "3016:save-then-close": "W11-030;W11-033;W11-034",
                "3016:shared-to-local-switch": "W11-031;W11-034",
                "3016:local-to-shared-switch": "W11-031;W11-034",
                "3016:shared-to-local-number": "W11-031;W11-034",
                "3016:local-to-shared-number": "W11-031;W11-034",
                "3016:shared-to-local-string": "W11-032;W11-034",
                "3016:local-to-shared-string": "W11-032;W11-034",
                "3016:unknown-operation-or-type": "W11-034",
                "3016:invalid-copy-guard-and-overflow": "W11-031;W11-032;W11-034",
            }
            for branch in inv["effective_branch_ids"].split(";"):
                rows.append(base | {
                    "ID": branch,
                    "variant": branch,
                    "parameters_and_modes": "Operation, type and packed source branch; see fixed 3016 scope audit",
                    "acceptance_ids": acceptance_by_branch[branch],
                })
        for field, mode, meaning in branches:
            if opcode in {3015, 3016, 3017, 3018, 3020, 3021, 3028, 3029, 3036} and inv.get("effective_branch_ids"):
                continue
            row = base | {
                "ID": f"{opcode}:{field.get('arg', field.get('from_arg', '?'))}:{mode}",
                "variant": f"{field.get('name', '?')}={mode}",
                "parameters_and_modes": flatten(meaning),
            }
            if opcode == 3007 and inv.get("effective_branch_ids"):
                row["acceptance_ids"] = "W06-004;W06-006"
            if opcode == 3008 and inv.get("effective_branch_ids"):
                row["acceptance_ids"] = (
                    "W06-008;W06-009" if field.get("arg") == 0 else
                    "W06-009;W06-010" if mode == "3" else "W06-008;W06-010")
            if opcode == 3006 and inv.get("effective_branch_ids") and field.get("name") == "shared_coordinate_mode":
                row["acceptance_ids"] = "W10-007"
                row["variant"] = ("A0=0 literal X/Y runtime branch" if mode == "0"
                                  else "A0 nonzero one-lookup X/Y runtime branch")
                row["parameters_and_modes"] = (
                    "A1/A2 signed literal X/Y; missing words zero; A3 bit0 coordinate branch independently applies"
                    if mode == "0" else
                    "Any nonzero A0; negative A1/A2 current-self, positive global, invalid/zero source zero; A3 bit0 independently applies")
            if opcode == 3023 and inv.get("effective_branch_ids"):
                row["variant"] = "formatter-only: " + row["variant"]
                row["parameters_and_modes"] = "formatter-only: " + row["parameters_and_modes"]
                row["gap_category"] = "formatter-only; not an effective runtime branch"
            if opcode == 3013 and field.get("name") == "operation":
                if inv.get("effective_branch_ids"):
                    operation = int(mode)
                    row["acceptance_ids"] = (
                        "W03-011" if operation in {0, 1, 21, 22}
                        else "W03-012" if operation in {2, 3, 17, 18}
                        else "W03-013" if operation in {4, 19}
                        else "W03-014" if operation in {5, 20}
                        else "W03-015" if 6 <= operation <= 15
                        else "W03-016")
                if int(mode) in {4, 19}:
                    row["actual_result"] = "Player 共用 250921 LCG 并经固定种子专项测试；原版启动种子和跨命令轨迹待验"
                elif int(mode) in {2, 3}:
                    row["actual_result"] = "正地址排序有 Player 专项；负 self 原版越界路径安全跳过"
            if opcode == 3019 and field.get("name") == "argument_source" and inv.get("effective_branch_ids"):
                row["acceptance_ids"] = {"0": "W03-006", "1": "W03-006", "2": "W03-006",
                                         "3": "W03-007", "4": "W03-008"}[mode]
                row["parameters_and_modes"] = (
                    "indirect signed array base; includes fallback modes 5..15" if mode == "2" else flatten(meaning))
                row["actual_result"] += (
                    "; W02-029 inline literal examples accepted" if mode == "3"
                    else "; W02-030 non-inline 1005 examples accepted")
            if opcode == 3036 and field.get("name") == "operation" and mode in {"0", "1", "2"}:
                row["actual_result"] = (
                    "Windows 250921 与 Player 当前地图事件数值 self 槽固定对照已通过；"
                    "全局观察槽0–7=7,7,42,42,99,0,99,0；"
                    "全局开关槽0复制到槽1后普通读取为2，self开关-1复制到全局槽2后为3，"
                    "Clear槽0后槽1/2保留。其它环境/类型/范围仍待验"
                )
                row["acceptance_example"] = "docs/probe-maniac-original-slot-zero.ps1；再扩充其它有效模式"
            rows.append(row)

    acceptance = list(csv.DictReader(ACCEPTANCE.open(encoding="utf-8-sig", newline=""), delimiter="\t"))
    by_acceptance_id = {item["id"]: item for item in acceptance}
    if set(ACCEPTED_OPCODE) - set(by_acceptance_id):
        raise ValueError("accepted opcode mapping references missing acceptance items")
    for key, opcode in ACCEPTED_OPCODE.items():
        item = by_acceptance_id[key]
        if item["status"] != "closed" or item["validated"] != "1":
            continue
        row = dict.fromkeys(HEADERS, "")
        row.update({
            "ID": key,
            "opcode": opcode,
            "variant": item["behavior_scope"],
            "work_package": item["work_package"],
            "reference_version_hash": f"250921:{runtime_hash}",
            "scenario_context": "固定自动地图事件及其公共事件调用；3002 另有原版存档快照",
            "platform": "Windows",
            "parameters_and_modes": item["behavior_scope"],
            "player_implementation": by_opcode[opcode]["player_handler"] or by_opcode[opcode]["player_dispatch"],
            "evidence": item["evidence"],
            "gap_category": "局部分支闭合",
            "dependencies": item["dependencies"],
            "acceptance_example": item["completion_condition"],
            "acceptance_ids": key,
            "status": "验证通过",
            "actual_result": ("250921 原版 3002 存档不含保存后写入的 ID37，进程内该值为 1；"
                              "只覆盖此赋值时点" if key == "W11-008" else
                              "250921 原版首次 3002 后向地图 self -6 写入 1；第二次存档的地图 self 子块增长；"
                              "Player 异步交付按调用帧身份写入成功 1、失败 0" if key == "W11-009" else
                              "固定 250921 原版轨迹与 Player 专项一致；只覆盖本行的行为范围"),
        })
        rows.append(row)

    generated_ids = {row["ID"] for row in rows}
    rows = [reviewed.get(row["ID"], row) for row in rows]
    for row in rows:
        if row["ID"] == "1005:command" and by_opcode[1005]["scope_audit_id"] == "C250921-01005":
            row["acceptance_ids"] = by_opcode[1005]["acceptance_ids"]
            row["scenario_context"] = by_opcode[1005]["applicable_contexts"]
            row["gap_category"] = "250921_windows_four_context_feature_validated"
            row["status"] = "verified"
            row["lifecycle_and_wait"] = "Fresh kind-0x25 child frame; four-context 1005 wait and original/Player save/restart behavior validated"
            row["save_fields"] = "Map-auto/parallel/nested active frames, cursors, positive wait and common bank survive restart; battle LSD omits active battle/common stack in both runtimes and restores map-only state"
            row["actual_result"] = "Valid-ID, frame, error, 3019 producer and four-context active-wait/restart differentials accepted; W02-024 complete"
            for reference in ("docs/verify-maniac-common1005-wait.py",
                              "docs/evidence/maniac-250921-common1005-wait-auto-audit.json",
                              "docs/evidence/maniac-250921-common1005-wait-parallel-audit.json",
                              "docs/evidence/maniac-250921-common1005-wait-nested-audit.json",
                              "docs/verify-maniac-common1005-battle-wait.py",
                              "docs/evidence/maniac-250921-common1005-wait-battle-audit.json"):
                if reference not in row["evidence"].split(";"):
                    row["evidence"] += ";" + reference
        if row["ID"] == "12330:command" and by_opcode[12330]["scope_audit_id"] == "C250921-12330":
            row["acceptance_ids"] = by_opcode[12330]["acceptance_ids"]
            row["scenario_context"] = by_opcode[12330]["applicable_contexts"]
            row["parameters_and_modes"] = "A0 selector 0/unknown,1..7; A3 offset/receiver nibbles; A4 offset, A5 raw receiver, A6 typed-pair count; Text shared literal name/string cursor"
            row["lifecycle_and_wait"] = "12330 allocates zeroed frame, resolves target and assigns persistent self-bank arguments before append; 12310 later delivers saved receiver; wait/restart remains open"
            row["save_fields"] = "A3 receiver nibble/A5 raw address saved for records length >=6; map requested ID and definition/self ID differ for cloned event; full four-context restart open"
            row["actual_result"] = "250921 Windows machine scope audited; W02-001/004 local original/Player common-event samples accepted, remaining feature leaves open"
            row["gap_category"] = "250921_windows_scope_audited_partial_feature_evidence"
            row["status"] = "partially_verified"
            for reference in ("docs/audit-maniac-12330-scope.py",
                              "docs/evidence/maniac-250921-12330-scope-audit.json"):
                if reference not in row["evidence"].split(";"):
                    row["evidence"] += ";" + reference
        if row["ID"].startswith("12330:0:") and by_opcode[12330]["scope_audit_id"] == "C250921-12330":
            selector = int(row["ID"].rsplit(":", 1)[1])
            row["acceptance_ids"] = ("W02-037;W02-038" if selector in (1, 2) else
                                     "W02-004;W02-040" if selector in (5, 6, 7) else
                                     "W02-004;W02-040")
            row["actual_result"] = ("Map selector feature remains open" if selector in (1, 2) else
                                    "Fixed W02-004 selector sample accepted; complete name/offset boundaries remain W02-040")
        if row["ID"].startswith("12330:7+2*i:") and by_opcode[12330]["scope_audit_id"] == "C250921-12330":
            row["acceptance_ids"] = "W02-001;W02-041"
            row["actual_result"] = "Fixed W02-001 numeric/switch sample accepted; all typed sources and recursive aliasing remain W02-041"
    if by_opcode[12330]["scope_audit_id"] == "C250921-12330":
        branch_acceptance = {
            "12330:common-selector-literal-variable-indirect-unknown": "W02-004;W02-040;W02-042",
            "12330:map-selector-literal-current-variable-page": "W02-037",
            "12330:map-clone-definition-and-page-error": "W02-038",
            "12330:common-name-literal-cache-and-text-budget": "W02-040",
            "12330:common-name-string-direct-indirect": "W02-004;W02-040",
            "12330:id-offset-literal-variable-indirect": "W02-004;W02-040",
            "12330:typed-number-string-switch-argument-sources": "W02-001;W02-041",
            "12330:typed-argument-sequential-alias-and-bank": "W02-001;W02-041",
            "12330:short-count-extra-unknown-record": "W02-042",
            "12330:zeroed-frame-depth-and-error-order": "W02-042",
            "12330:deferred-return-destination": "W02-043",
            "12330:four-context-call-wait-save-continuation": "W02-046;W02-047;W02-048;W02-049",
            "12330:3019-produced-call-record": "W03-009",
        }
        command = next(row for row in rows if row["ID"] == "12330:command")
        for branch, acceptance in branch_acceptance.items():
            rows.append(command | {"ID": branch, "variant": branch,
                                   "acceptance_ids": acceptance, "status": "scope_audited",
                                   "actual_result": "Effective original branch pinned; linked Player feature evidence remains limited to its accepted leaf IDs"})
    generated_ids = {row["ID"] for row in rows}
    rows.extend(previous for key, previous in reviewed.items()
                if key not in generated_ids)
    ids = [row["ID"] for row in rows]
    if len(ids) != len(set(ids)) or len({row["opcode"] for row in rows}) != 168:
        duplicates = sorted({key for key in ids if ids.count(key) > 1})
        raise ValueError(f"ledger IDs or opcode coverage invalid: duplicate={duplicates}, opcode_count={len({row['opcode'] for row in rows})}")
    with OUTPUT.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=HEADERS)
        writer.writeheader()
        writer.writerows(rows)
    print(f"{OUTPUT}: {len(rows)} rows, 168 opcodes, {len(rows) - 168 - len(ACCEPTED_OPCODE)} field-mode candidates, {len(ACCEPTED_OPCODE)} linked closed branches")

    # Textual reference inventory: declarations and definitions remain visible so
    # an auditor can identify every route into the shared helpers.
    helper_names = (
        "DecodeTargetEvaluationMode", "ValueOrVariableBitfield",
        "ValueOrVariable", "CommandStringOrVariableBitfield",
        "CommandStringOrVariable", "GetWithMode",
    )
    pattern = re.compile(r"\b(" + "|".join(helper_names) + r")\b")
    method_pattern = re.compile(
        r"^\s*(?!return\b|if\b|while\b|switch\b)(?:[\w:<>,*&]+\s+)+"
        r"(Game_Interpreter(?:_Map|_Battle)?::[A-Za-z0-9_]+)\s*\(")
    references = []
    indirect_callers = {}
    for source in sorted((ROOT / "src").rglob("*")):
        if source.suffix not in {".cpp", ".h"}:
            continue
        owner = ""
        for line_number, line in enumerate(source.read_text(encoding="utf-8-sig").splitlines(), 1):
            owner_match = method_pattern.search(line)
            if owner_match:
                owner = owner_match.group(1)
            if "OperateValue(" in line and not owner.endswith("::OperateValue") and "::Command" in owner:
                indirect_callers.setdefault("Game_Interpreter::OperateValue", set()).add(owner)
            for helper in set(pattern.findall(line)):
                stripped = line.strip()
                if stripped.startswith("//"):
                    kind = "comment"
                elif stripped.startswith("template "):
                    kind = "explicit_template_instantiation"
                elif source.name == "game_interpreter_shared.cpp" and "Game_Interpreter_Shared::" in line and not stripped.startswith("return "):
                    kind = "shared_definition"
                elif source.name == "game_interpreter_shared.h" and stripped.endswith(";") and not stripped.startswith("return "):
                    kind = "declaration"
                elif source.name in {"game_strings.h", "game_variables.h"}:
                    kind = "storage_helper_definition_or_declaration"
                elif source.name == "game_interpreter_shared.h":
                    kind = "interpreter_wrapper"
                elif source.name == "game_interpreter_shared.cpp":
                    kind = "shared_internal_call"
                else:
                    kind = "consumer_call"
                references.append({
                    "helper": helper,
                    "file": source.relative_to(ROOT).as_posix(),
                    "line": line_number,
                    "source_excerpt": line.strip(),
                    "reference_kind": kind,
                    "owner_function": owner if kind == "consumer_call" else "",
                    "review_status": "待核对局部模式语义" if kind == "consumer_call" else "非外部消费者",
                })
    with HELPERS_OUTPUT.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(references[0]))
        writer.writeheader()
        writer.writerows(references)
    print(f"{HELPERS_OUTPUT}: {len(references)} textual references")

    # This is a source-level consumer audit, not a claim that the original
    # runtime accepts every mode exposed by the shared helper.
    by_handler = {}
    for opcode, item in by_opcode.items():
        handler = item["player_handler"]
        if handler:
            by_handler.setdefault(handler, []).append((opcode, work[opcode]))
    source_cache = {}
    consumers = []
    for reference in references:
        if reference["reference_kind"] != "consumer_call":
            continue
        source = source_cache.setdefault(reference["file"], (ROOT / reference["file"]).read_text(encoding="utf-8-sig"))
        invocation, args, prefix = helper_call(source, reference["line"], reference["helper"])
        mode, shift, operand, rule = describe_helper_call(reference["helper"], args)
        if not prefix and reference["helper"] == "DecodeTargetEvaluationMode":
            prefix = "standalone call; boolean result discarded"
        if not prefix and reference["helper"] == "ValueOrVariableBitfield" and reference["owner_function"].endswith("CommandChangeActorFace"):
            prefix = "second argument of actor->SetFace"
        owner_function = reference["owner_function"]
        command_handlers = sorted(indirect_callers.get(owner_function, {owner_function}))
        owners = [entry for handler in command_handlers for entry in by_handler.get(handler, [])]
        consumers.append({
            "helper": reference["helper"],
            "file": reference["file"],
            "line": reference["line"],
            "owner_function": owner_function,
            "command_handlers": ";".join(command_handlers),
            "inventory_opcodes": ";".join(str(opcode) for opcode, _ in owners),
            "work_packages": ";".join(dict.fromkeys(package for _, package in owners)),
            "call": invocation,
            "mode_source": mode,
            "bitfield_nibble": shift,
            "operand_source": operand,
            "result_context": prefix,
            "local_rule": rule,
            "review_boundary": "Player source rule audited; original per-command modes and result semantics remain in work-package acceptance",
        })
    if len(consumers) != sum(reference["reference_kind"] == "consumer_call" for reference in references):
        raise ValueError("shared helper consumer calls were omitted")
    with HELPER_CONSUMERS_OUTPUT.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(consumers[0]))
        writer.writeheader()
        writer.writerows(consumers)
    print(f"{HELPER_CONSUMERS_OUTPUT}: {len(consumers)} source-level consumers")


if __name__ == "__main__":
    main()
