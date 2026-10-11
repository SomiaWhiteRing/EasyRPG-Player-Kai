param(
    [string]$Fixture = (Join-Path $PSScriptRoot '..\build\maniac-original-241028-controlvars'),
    [string]$Output = (Join-Path $PSScriptRoot '..\build\maniac-original-241028-controlvars-trace.json'),
    [int]$MaximumEnters = 16,
    [int]$AdditionalSecondsAfterSave = 0,
    [int]$AdditionalEntersAfterSave = 0,
    [string]$RuntimeExecutable = '',
    [string[]]$RuntimeArguments = @()
)

$ErrorActionPreference = 'Stop'
$fixturePath = (Resolve-Path -LiteralPath $Fixture).Path
$manifestPath = Join-Path $fixturePath 'manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($null -eq $manifest.expected_save_slot) { throw 'Fixture has no expected save slot.' }
foreach ($file in $manifest.files.PSObject.Properties) {
    $path = Join-Path $fixturePath $file.Name
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $file.Value) { throw "Fixture input changed: $($file.Name)" }
}
$exe = if ($RuntimeExecutable) { (Resolve-Path -LiteralPath $RuntimeExecutable).Path } else { Join-Path $fixturePath 'RPG_RT.exe' }
$save = Join-Path $fixturePath ('Save{0:D2}.lsd' -f [int]$manifest.expected_save_slot)
if (Test-Path -LiteralPath $save) { Remove-Item -LiteralPath $save }

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ManiacSaveProbeNative {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
}
'@

$process = if ($RuntimeArguments.Count -gt 0) {
    Start-Process -FilePath $exe -ArgumentList $RuntimeArguments -WorkingDirectory $fixturePath -WindowStyle Hidden -PassThru
} else {
    Start-Process -FilePath $exe -WorkingDirectory $fixturePath -WindowStyle Hidden -PassThru
}
try {
    Start-Sleep -Seconds 2
    $process.Refresh()
    if ($process.HasExited) {
        throw "Runtime exited before saving (exit code $($process.ExitCode))."
    }
    if (-not $RuntimeExecutable -and $process.MainWindowHandle -eq [IntPtr]::Zero) {
        throw 'Original runtime did not create its game window.'
    }
    $foundAt = $null
    for ($step = 1; $step -le $MaximumEnters; $step++) {
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            [void][ManiacSaveProbeNative]::SetForegroundWindow($process.MainWindowHandle)
            [ManiacSaveProbeNative]::keybd_event(13, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 80
            [ManiacSaveProbeNative]::keybd_event(13, 0, 2, [UIntPtr]::Zero)
        }
        Start-Sleep -Milliseconds 500
        if (Test-Path -LiteralPath $save) {
            $foundAt = $step
            break
        }
    }
    if ($null -eq $foundAt) { throw 'Original runtime did not produce the expected save.' }
    $saveHash = $null
    $saveLength = $null
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        try {
            $lengthBefore = (Get-Item -LiteralPath $save).Length
            $hash = (Get-FileHash -LiteralPath $save -Algorithm SHA256).Hash.ToLowerInvariant()
            Start-Sleep -Milliseconds 150
            $lengthAfter = (Get-Item -LiteralPath $save).Length
            if ($lengthBefore -eq $lengthAfter -and $lengthAfter -gt 0) {
                $saveHash = $hash
                $saveLength = $lengthAfter
                break
            }
        } catch {
            Start-Sleep -Milliseconds 150
        }
    }
    if ($null -eq $saveHash) { throw 'Original save never became readable and stable.' }
    for ($step = 0; $step -lt $AdditionalEntersAfterSave; $step++) {
        $process.Refresh()
        if ($process.HasExited) { break }
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            [void][ManiacSaveProbeNative]::SetForegroundWindow($process.MainWindowHandle)
            [ManiacSaveProbeNative]::keybd_event(13, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 80
            [ManiacSaveProbeNative]::keybd_event(13, 0, 2, [UIntPtr]::Zero)
        }
        Start-Sleep -Milliseconds 500
    }
    if ($AdditionalSecondsAfterSave -gt 0) {
        Start-Sleep -Seconds $AdditionalSecondsAfterSave
    }
    # Auto-start events can save again while the probe is reading the first file.
    # Bind the trace to the bytes left on disk after the runtime is stopped.
    $process.Refresh()
    if (-not $process.HasExited) {
        Stop-Process -Id $process.Id -Force
        $process.WaitForExit()
    }
    $saveLength = (Get-Item -LiteralPath $save).Length
    $saveHash = (Get-FileHash -LiteralPath $save -Algorithm SHA256).Hash.ToLowerInvariant()
    $result = [ordered]@{
        reference_sha256 = if ($RuntimeExecutable) { $manifest.files.'RPG_RT.exe' } else { (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant() }
        fixture_manifest_sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
        save_file_sha256 = $saveHash
        save_file_length = $saveLength
        save_slot = [int]$manifest.expected_save_slot
        save_observed_after_enter = $foundAt
        additional_enters_after_save = $AdditionalEntersAfterSave
        additional_seconds_after_save = $AdditionalSecondsAfterSave
    }
    if ($RuntimeExecutable) {
        $result.player_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
        $result.player_arguments = $RuntimeArguments
    }
    $json = $result | ConvertTo-Json -Depth 4
    $json | Set-Content -LiteralPath $Output -Encoding utf8
    Write-Output $json
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
