# Maniac compatibility work

Follow [the fixed Maniac action guidelines](docs/research/maniac-250921/ACTION-GUIDELINES.md)
when screening, migrating, repairing or estimating Maniac support. They replace
the old full-port completion gates; preserve EasyRPG fonts, input and host APIs.

# Windows local builds

When the user explicitly requests or authorizes a local Windows build in the
current task, use `builds/cmake/build-windows.ps1` for Player's CMake targets.
For source-editing requests, the default verification is static inspection of
the changes; report the checks actually performed with the result.
Pass the existing ASCII build-directory alias; the script reuses its cached
source and CMake executable. For example:

```powershell
& ./builds/cmake/build-windows.ps1 -BuildDirectory C:/buildscripts/mr14/player-build -Targets @('EasyRPG_Player_exe', 'test_runner_player') -ConfigureArguments @('-DPLAYER_BUILD_LIBLCF=ON') -Parallel 4
```

The local `lib/liblcf` checkout must include
`builds/patches/liblcf-maniac-250921.patch`. If the wrapper reports stale
headers, inspect its local changes, run `git apply --check` for that patch,
then apply it without resetting the checkout. This checkout uses the in-tree
compatibility-patched dependency. A reverse apply check confirms the patch is present;
do not apply it twice. Keep `PLAYER_BUILD_LIBLCF=ON` for this source dependency.
Setting `liblcf_DIR` alone does not select a prebuilt install while that option
is ON. Check the selected dependency in configuration output, then compile.

Do not invoke CMake from the repository's non-ASCII working directory. ASCII
`-S` and `-B` arguments alone are insufficient on this machine. The wrapper
keeps both configuration and compilation in the ASCII build directory and
restores the caller's location. Do not retry unchanged failing configurations
or alter global PATH/profile settings to work around this failure.
