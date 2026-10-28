[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $BuildDirectory,
	[string] $SourceDirectory,
	[string] $CMakeExecutable,
	[string] $Configuration = "Release",
	[string[]] $Targets = @("EasyRPG_Player_exe"),
	[int] $Parallel = [Environment]::ProcessorCount,
	[string[]] $ConfigureArguments = @()
)

$ErrorActionPreference = "Stop"
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
if ($BuildDirectory -match '[^\x00-\x7f]') {
	throw "Use an ASCII build-directory path (a junction is supported). CMake's working directory must also be ASCII."
}

$cache = Join-Path $BuildDirectory "CMakeCache.txt"
$cacheLines = if (Test-Path -LiteralPath $cache) { Get-Content -LiteralPath $cache } else { @() }
if (-not $SourceDirectory) {
	$cachedSource = $cacheLines | Where-Object { $_ -match '^CMAKE_HOME_DIRECTORY:INTERNAL=' } | Select-Object -First 1
	$SourceDirectory = if ($cachedSource) { $cachedSource.Split('=', 2)[1] } else { Join-Path $PSScriptRoot "..\.." }
}
if (-not $CMakeExecutable) {
	$cachedCMake = $cacheLines | Where-Object { $_ -match '^CMAKE_COMMAND:INTERNAL=' } | Select-Object -First 1
	$CMakeExecutable = if ($cachedCMake) { $cachedCMake.Split('=', 2)[1] } else { (Get-Command cmake.exe -ErrorAction Stop).Source }
}

$SourceDirectory = [IO.Path]::GetFullPath($SourceDirectory)
if (-not (Test-Path -LiteralPath (Join-Path $SourceDirectory "CMakeLists.txt"))) {
	throw "Missing Player source: $SourceDirectory"
}
if (-not (Test-Path -LiteralPath $CMakeExecutable -PathType Leaf)) {
	throw "Missing CMake executable: $CMakeExecutable"
}

# An existing lib/liblcf checkout wins over the pinned dependency when this
# option is enabled. Catch the known stale-checkout mismatch before compiling.
$buildLcf = $cacheLines | Where-Object { $_ -match '^PLAYER_BUILD_LIBLCF:[^=]+=' } | Select-Object -First 1
$buildLcf = if ($buildLcf) { $buildLcf.Split('=', 2)[1] } else { "OFF" }
foreach ($argument in $ConfigureArguments) {
	if ($argument -match '^-DPLAYER_BUILD_LIBLCF(?::BOOL)?=(.+)$') { $buildLcf = $Matches[1] }
}
$vendoredSaveSystem = Join-Path $SourceDirectory "lib/liblcf/src/generated/lcf/rpg/savesystem.h"
if ($buildLcf -match '^(ON|TRUE|YES|1)$' -and (Test-Path -LiteralPath $vendoredSaveSystem)) {
	if (-not (Select-String -LiteralPath $vendoredSaveSystem -SimpleMatch 'maniac_face_animation_flags' -Quiet)) {
		throw "The existing lib/liblcf checkout lacks required Maniac fields. Inspect its local changes, check and apply builds/patches/liblcf-maniac-250921.patch without resetting the checkout, then keep PLAYER_BUILD_LIBLCF=ON. See the Windows build guide."
	}
}
New-Item -ItemType Directory -Path $BuildDirectory -Force | Out-Null

# Give a fresh build an ASCII source alias without copying or resetting files.
if ($SourceDirectory -match '[^\x00-\x7f]') {
	$sourceAlias = Join-Path $BuildDirectory "source"
	if (Test-Path -LiteralPath $sourceAlias) {
		$link = Get-Item -LiteralPath $sourceAlias
		if ($link.LinkType -ne "Junction" -or $link.Target -ne $SourceDirectory) {
			throw "Source alias belongs to another directory: $sourceAlias"
		}
	} else {
		New-Item -ItemType Junction -Path $sourceAlias -Target $SourceDirectory | Out-Null
	}
	$SourceDirectory = $sourceAlias
}

Write-Host "CMake: $CMakeExecutable"
Write-Host "Source: $SourceDirectory"
Write-Host "Build and working directory: $BuildDirectory"
Push-Location -LiteralPath $BuildDirectory
try {
	# ASCII -S/-B paths alone do not prevent the Windows lookup crash when
	# CMake starts in a non-ASCII working directory. Keep this for both calls.
	& $CMakeExecutable -S $SourceDirectory -B $BuildDirectory @ConfigureArguments
	if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed with exit code $LASTEXITCODE." }
	& $CMakeExecutable --build $BuildDirectory --config $Configuration --target @Targets --parallel $Parallel
	if ($LASTEXITCODE -ne 0) { throw "CMake build failed with exit code $LASTEXITCODE." }
} finally {
	Pop-Location
}
