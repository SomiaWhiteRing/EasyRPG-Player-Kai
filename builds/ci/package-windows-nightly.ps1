param(
	[string] $BuildDir = $(if ($env:BUILD_DIR) { $env:BUILD_DIR } else { "build/windows-x64-vs2022-release" }),
	[string] $BuildscriptsDir = $(if ($env:BUILDSCRIPTS_DIR) { $env:BUILDSCRIPTS_DIR } else { "external/buildscripts" }),
	[string] $VcpkgTriplet = $(if ($env:VCPKG_TRIPLET) { $env:VCPKG_TRIPLET } else { "x64-windows-static" }),
	[string] $ToolchainUrl = $(if ($env:TOOLCHAIN_URL) { $env:TOOLCHAIN_URL } else { "https://ci.easyrpg.org/downloads/windows/toolchain-windows.zip" }),
	[string] $ArtifactDir = $(if ($env:ARTIFACT_DIR) { $env:ARTIFACT_DIR } else { "build/artifacts" }),
	[string] $WindowsAssetName = $(if ($env:WINDOWS_ASSET_NAME) { $env:WINDOWS_ASSET_NAME } else { "Player.exe" })
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $repoRoot

if (-not [System.IO.Path]::IsPathRooted($BuildDir)) {
	$BuildDir = Join-Path $repoRoot $BuildDir
}
if (-not [System.IO.Path]::IsPathRooted($BuildscriptsDir)) {
	$BuildscriptsDir = Join-Path $repoRoot $BuildscriptsDir
}
if (-not [System.IO.Path]::IsPathRooted($ArtifactDir)) {
	$ArtifactDir = Join-Path $repoRoot $ArtifactDir
}

Write-Host "==> Resolving CMake"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$cmakePath = ""

if (Test-Path $vswhere) {
	$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
	if ($vsPath) {
		$candidate = Join-Path $vsPath "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
		if (Test-Path $candidate) {
			$cmakePath = $candidate
		}
	}
}

if (-not $cmakePath) {
	$cmakePath = (Get-Command cmake.exe -ErrorAction Stop).Source
}

& $cmakePath --version

Write-Host "==> Preparing EasyRPG Windows toolchain"
$toolchainFile = Join-Path $BuildscriptsDir "windows\vcpkg\scripts\buildsystems\vcpkg.cmake"
$tripletDir = Join-Path $BuildscriptsDir "windows\vcpkg\installed\$VcpkgTriplet"

if (-not (Test-Path $toolchainFile)) {
	$windowsDir = Join-Path $BuildscriptsDir "windows"
	$vcpkgDir = Join-Path $windowsDir "vcpkg"
	$zipPath = Join-Path ([System.IO.Path]::GetTempPath()) "toolchain-windows.zip"

	New-Item -ItemType Directory -Force -Path $windowsDir | Out-Null
	Remove-Item $vcpkgDir -Recurse -Force -ErrorAction SilentlyContinue

	curl.exe -L --fail --retry 5 --retry-delay 10 -o $zipPath $ToolchainUrl
	if ($LASTEXITCODE -ne 0) {
		throw "Failed to download $ToolchainUrl"
	}

	New-Item -ItemType Directory -Force -Path $vcpkgDir | Out-Null
	tar.exe -xf $zipPath -C $vcpkgDir --strip-components=1
	if ($LASTEXITCODE -ne 0) {
		throw "Failed to extract $zipPath"
	}
}

if (-not (Test-Path $toolchainFile)) {
	throw "Missing vcpkg toolchain: $toolchainFile"
}
if (-not (Test-Path $tripletDir)) {
	throw "Missing vcpkg triplet directory: $tripletDir"
}
if (-not (Test-Path (Join-Path $tripletDir "share\sdl2\SDL2Config.cmake"))) {
	throw "Missing SDL2 package in vcpkg triplet: $tripletDir"
}

Get-ChildItem (Join-Path $BuildscriptsDir "windows\vcpkg\installed") -Directory |
	Select-Object -ExpandProperty Name

Write-Host "==> Configuring Windows build"
$versionSuffix = "(nightly, $(Get-Date -AsUTC -Format 'yyyy-MM-dd'))"
$versionArg = "-DPLAYER_VERSION_APPEND=$versionSuffix"
$tripletArg = "-DVCPKG_TARGET_TRIPLET=$VcpkgTriplet"
$configureOutput = & $cmakePath -S $repoRoot -B $BuildDir `
	-G "Visual Studio 17 2022" -A x64 `
	-DCMAKE_BUILD_TYPE=Release `
	-DCMAKE_TOOLCHAIN_FILE="$toolchainFile" `
	$tripletArg `
	-DPLAYER_BUILD_LIBLCF=ON `
	-DPLAYER_ENABLE_TESTS=OFF `
	$versionArg 2>&1
$configureExit = $LASTEXITCODE

$configureOutput | ForEach-Object { $_ }

if ($configureExit -ne 0) {
	Get-ChildItem $BuildDir -Recurse -Include CMakeOutput.log,CMakeError.log -ErrorAction SilentlyContinue |
		ForEach-Object {
			Write-Host "::group::$($_.FullName)"
			Get-Content $_.FullName -ErrorAction SilentlyContinue
			Write-Host "::endgroup::"
		}

	exit $configureExit
}

Write-Host "==> Building Windows executable"
& $cmakePath --build $BuildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) {
	exit $LASTEXITCODE
}

Write-Host "==> Verifying Windows executable"
$exePath = Join-Path $BuildDir "Release\Player.exe"
if (-not (Test-Path $exePath)) {
	throw "Missing build output: $exePath"
}

$dlls = Get-ChildItem (Split-Path $exePath) -Filter *.dll -File -ErrorAction SilentlyContinue
if ($dlls) {
	$names = ($dlls | Select-Object -ExpandProperty Name) -join ", "
	throw "Release build is expected to be a single-file static build, but DLLs were produced: $names"
}

& $exePath --version

Write-Host "==> Staging Windows artifact"
New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null
$assetPath = Join-Path $ArtifactDir $WindowsAssetName
Copy-Item $exePath -Destination $assetPath -Force

if ($env:GITHUB_OUTPUT) {
	"artifact_path=$assetPath" >> $env:GITHUB_OUTPUT
}

Write-Host "Windows artifact: $assetPath"
