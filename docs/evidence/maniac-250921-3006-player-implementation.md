# 3006 Player implementation checkpoint (W10-007)

Date: 2026-09-26. Scope: 250921 Windows Set Mouse Position operands and cursor path. This is implementation evidence, not feature acceptance. The fixed original executable and consumer are identified in `maniac-250921-3006-scope-audit.json` (runtime SHA-256 `082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f`). The 241028 consumer difference is recorded in `maniac-241028-3006-consumer-audit.json`.

| Behavior | Fixed original consumer | Player after this change | Verification |
|---|---|---|---|
| A0=0 | A1/A2 are signed literal X/Y | Reads literal A1/A2 in X then Y order | Code review and Windows build; no live comparison |
| A0!=0 | Each A1/A2 is one global or current-self lookup; missing/zero/out-of-range reads zero | Uses `ReadVariable` once per coordinate. Its self reader negates in int64, including INT_MIN | Code review; boundary and alias cases await runtime comparison |
| Short and extra records | Count 0..2 defaults missing operands to zero; A3 read only when count>=4; other Args/Text ignored | Dispatch uses `CmdSetup<...,0>` and bounds-checks each operand without changing the command | Code review; serialized record retention awaits comparison |
| Scaled coordinates | Clamp to [0,319]/[0,239], divide by 320/client width and 240/client height factors, nearbyint; nonpositive dimensions use factor one | Windows native client rectangle and same operation order in `nearbyint` | Code review; real dimensions, DPI, black bars and rounding ties await comparison |
| Raw coordinates | Clamp to [0,S*320-1]/[0,S*240-1], where S is option scale clamped to [1,4] | Uses S=1, the zero-initialized native scale's effective value before 3038 option type 1 changes it | Option 1 remains unsupported in Player; this branch is incomplete after an option change |
| OS cursor | Calls ClientToScreen and SetCursorPos without checking either result or focus | Native HWND path through BaseUi performs those two calls in order, without testing return values or focus | Build only; actual pointer and OS failure behavior await runtime comparison |
| Other platforms | Original is Windows | BaseUi returns unsupported; command logs once and continues | Web/Android policies and runtime checks remain open |

Existing checks: Visual Studio 2022 Debug SDL2 `EasyRPG_Player_exe` and `test_runner_player` built successfully; `test_runner.exe --no-colors=true --test-suite=Game_Interpreter --no-skip=true` passed 42 cases and 970 assertions. The initial build attempted with the system CMake 4.2 failed during regeneration because its modules were mixed with Visual Studio CMake 3.31; rebuilding with Visual Studio's bundled CMake succeeded. No manual UI comparison or original runtime cursor probe was run in this checkpoint.

W10-007 remains open. Required next evidence: paired original/Player cursor measurements for count 0..4 and extra Args/Text, literal/nonzero modes, positive/negative/zero/out-of-range/INT_MIN addresses and aliases, scaled/raw clamp and tie rounding, window sizes, DPI, black bars, focus and OS failures. Implement and compare 3038 option type 1 scale changes before accepting raw coordinates. W10-008 owns scheduling and saved continuation; W12-012 owns the old/new full differential.

Correction (2026-09-26): the zoom state producer is opcode 3038, not opcode 3018. The original 3038 consumer at RVA 0x4d570 sets state 0x2702a4; 3006 reads it at RVA 0x24e19. The original 3018 option 1 controls FPS. See maniac-250921-3038-scope-audit.json.
