param(
	[ValidateSet("all", "web", "android")]
	[string] $Target = "all",

	[string] $Image = "easyrpg-player-kai-package:ubuntu24.04",

	[switch] $SkipImageBuild,

	[switch] $Pull
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$dockerContext = Join-Path (Join-Path $repoRoot "builds") "docker"
$dockerfile = Join-Path $dockerContext "package.Dockerfile"
$cacheRoot = Join-Path (Join-Path $repoRoot "external") "local-docker"
$isWindowsHost = [System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT
$androidSigningEnv = @(
	"ANDROID_RELEASE_KEYSTORE_BASE64",
	"ANDROID_RELEASE_STORE_PASSWORD",
	"ANDROID_RELEASE_KEY_ALIAS",
	"ANDROID_RELEASE_KEY_PASSWORD"
)

if ($Target -in @("all", "android")) {
	foreach ($name in $androidSigningEnv) {
		if (-not [System.Environment]::GetEnvironmentVariable($name)) {
			throw "Missing $name. See builds/android/release-signing.md."
		}
	}
}

function Get-ShortHash {
	param(
		[string] $Value
	)

	$sha256 = [System.Security.Cryptography.SHA256]::Create()
	try {
		$bytes = [System.Text.Encoding]::UTF8.GetBytes($Value.ToLowerInvariant())
		$hash = $sha256.ComputeHash($bytes)
		return (($hash[0..7] | ForEach-Object { $_.ToString("x2") }) -join "")
	} finally {
		$sha256.Dispose()
	}
}

function Convert-ProxyForDocker {
	param(
		[string] $Value
	)

	if (-not $Value) {
		return $null
	}

	if (-not $isWindowsHost) {
		return $Value
	}

	try {
		$uri = [System.Uri] $Value
		if ($uri.Host -in @("127.0.0.1", "localhost", "::1")) {
			$builder = [System.UriBuilder]::new($uri)
			$builder.Host = "host.docker.internal"
			return $builder.Uri.AbsoluteUri.TrimEnd("/")
		}
	} catch {
		return $Value
	}

	return $Value
}

Get-Command docker -ErrorAction Stop | Out-Null

& docker info --format "{{.ServerVersion}}" | Out-Null
if ($LASTEXITCODE -ne 0) {
	throw "Docker Desktop is not running or the docker CLI cannot reach it."
}

$logDir = Join-Path $repoRoot "build"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$logPath = Join-Path $logDir "package-docker-$Target.log"

@(
	$cacheRoot,
	(Join-Path $cacheRoot "home"),
	(Join-Path $cacheRoot "gradle"),
	(Join-Path $cacheRoot "web"),
	(Join-Path $cacheRoot "android")
) | ForEach-Object {
	New-Item -ItemType Directory -Force -Path $_ | Out-Null
}

if (-not $SkipImageBuild) {
	$buildArgs = @("build", "-f", $dockerfile, "-t", $Image)
	if ($Pull) {
		$buildArgs += "--pull"
	}
	$buildArgs += $dockerContext

	& docker @buildArgs
	if ($LASTEXITCODE -ne 0) {
		exit $LASTEXITCODE
	}
}

$containerEnvArgs = @(
	"-e", "HOME=/workspace/external/local-docker/home",
	"-e", "GRADLE_USER_HOME=/workspace/external/local-docker/gradle",
	"-e", "GITHUB_RUN_NUMBER=$env:GITHUB_RUN_NUMBER"
)

if ($Target -in @("all", "android")) {
	if (-not $isWindowsHost) {
		# Refresh build outputs separately from the large SDK/NDK snapshot in CI.
		$containerEnvArgs += @(
			"-e", "ANDROID_GRADLE_BUILD_ROOT=/workspace/external/local-docker/android-build",
			"-e", "CCACHE_DIR=/workspace/external/local-docker/android-build/ccache",
			"-e", "CCACHE_MAXSIZE=1G"
		)
	}
	foreach ($name in $androidSigningEnv) {
		# Docker inherits the value; keep secret values out of command-line arguments.
		$containerEnvArgs += @("-e", $name)
	}
}

$passthroughEnv = @(
	"WEB_ASSET_NAME",
	"ANDROID_ASSET_NAME",
	"ANDROID_ABIS",
	"ANDROID_NDK_VERSION",
	"BUILDSCRIPTS_REF",
	"VERSION_CODE_OVERRIDE"
)

foreach ($name in $passthroughEnv) {
	$value = [System.Environment]::GetEnvironmentVariable($name)
	if ($value) {
		$containerEnvArgs += @("-e", "${name}=${value}")
	}
}

$proxyEnv = @(
	@{ Name = "HTTP_PROXY"; Value = (Convert-ProxyForDocker ([System.Environment]::GetEnvironmentVariable("HTTP_PROXY"))) },
	@{ Name = "HTTPS_PROXY"; Value = (Convert-ProxyForDocker ([System.Environment]::GetEnvironmentVariable("HTTPS_PROXY"))) },
	@{ Name = "ALL_PROXY"; Value = (Convert-ProxyForDocker ([System.Environment]::GetEnvironmentVariable("ALL_PROXY"))) },
	@{ Name = "NO_PROXY"; Value = [System.Environment]::GetEnvironmentVariable("NO_PROXY") }
)

foreach ($proxy in $proxyEnv) {
	if ($proxy.Value) {
		$containerEnvArgs += @("-e", "$($proxy.Name)=$($proxy.Value)")
		$containerEnvArgs += @("-e", "$($proxy.Name.ToLowerInvariant())=$($proxy.Value)")
	}
}

$cacheMountArgs = @()
if ($isWindowsHost) {
	$cacheVolumePrefix = "easyrpg-player-kai-local-docker-$(Get-ShortHash $repoRoot)"
	$cacheVolumeMounts = @(
		@{ Name = "home"; Target = "/workspace/external/local-docker/home" },
		@{ Name = "gradle"; Target = "/workspace/external/local-docker/gradle" },
		@{ Name = "web"; Target = "/workspace/external/local-docker/web" },
		@{ Name = "android"; Target = "/workspace/external/local-docker/android" },
		@{ Name = "android-build"; Target = "/workspace/external/local-docker/android/gradle-build" }
	)

	Write-Host "Using Docker volumes for local build caches: $cacheVolumePrefix-*"
	foreach ($mount in $cacheVolumeMounts) {
		$volumeName = "$cacheVolumePrefix-$($mount.Name)"
		& docker volume create $volumeName | Out-Null
		if ($LASTEXITCODE -ne 0) {
			exit $LASTEXITCODE
		}
		$cacheMountArgs += @("--mount", "type=volume,source=$volumeName,target=$($mount.Target)")
	}
	$containerEnvArgs += @("-e", "ANDROID_GRADLE_BUILD_ROOT=/workspace/external/local-docker/android/gradle-build")
}

$runArgs = @(
	"run",
	"--rm",
	"--init"
) + $containerEnvArgs + @(
	"-v", "${repoRoot}:/workspace"
) + $cacheMountArgs + @(
	"-w", "/workspace",
	$Image,
	"bash",
	"./builds/ci/package-ubuntu-nightly.sh",
	$Target
)

if (-not $isWindowsHost) {
	$userId = (& id -u).Trim()
	$groupId = (& id -g).Trim()
	$runArgs = @("run", "--rm", "--init", "--user", "${userId}:${groupId}") + $containerEnvArgs + @(
		"-v", "${repoRoot}:/workspace",
		"-w", "/workspace",
		$Image,
		"bash",
		"./builds/ci/package-ubuntu-nightly.sh",
		$Target
	)
}

Write-Host "Writing Docker package log to: $logPath"
& docker @runArgs 2>&1 | Tee-Object -FilePath $logPath
$dockerRunExitCode = $LASTEXITCODE
if ($dockerRunExitCode -ne 0) {
	exit $dockerRunExitCode
}
