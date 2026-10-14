# Windows local builds

Use `builds/cmake/build-windows.ps1` for local Windows builds of Player's CMake targets.
Pass the existing ASCII build-directory alias; the script reuses its cached
source and CMake executable. For example:

```powershell
& ./builds/cmake/build-windows.ps1 -BuildDirectory C:/buildscripts/mr14/player-build -Targets @('EasyRPG_Player_exe', 'test_runner_player') -Parallel 4
```

Do not invoke CMake from the repository's non-ASCII working directory. ASCII
`-S` and `-B` arguments alone are insufficient on this machine. The wrapper
keeps both configuration and compilation in the ASCII build directory and
restores the caller's location. Do not retry unchanged failing configurations
or alter global PATH/profile settings to work around this failure.
