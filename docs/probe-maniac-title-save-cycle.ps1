param(
    [Parameter(Mandatory=$true)][string]$Fixture,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$RuntimeExecutable = '',
    [string[]]$RuntimeArguments = @(),
    [int]$MaximumEnters = 80
)

$ErrorActionPreference = 'Stop'
$fixturePath = (Resolve-Path -LiteralPath $Fixture).Path
$manifestPath = Join-Path $fixturePath 'manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.save_variant -ne 'title-cycle-save') { throw 'Expected title-cycle-save fixture.' }
foreach ($file in $manifest.files.PSObject.Properties) {
    $path = Join-Path $fixturePath $file.Name
    $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $file.Value) { throw "Fixture input changed: $($file.Name)" }
}
$exe = if ($RuntimeExecutable) { (Resolve-Path -LiteralPath $RuntimeExecutable).Path } else { Join-Path $fixturePath 'RPG_RT.exe' }
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$saves = @(1, 2) | ForEach-Object { Join-Path $fixturePath ('Save{0:D2}.lsd' -f $_) }
foreach ($save in $saves) { if (Test-Path -LiteralPath $save) { Remove-Item -LiteralPath $save } }

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ManiacTitleCycleKeys {
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
    $observedTimes = @{}
    $events = [System.Collections.Generic.List[object]]::new()
    for ($step = 0; $step -le $MaximumEnters; $step++) {
        $process.Refresh()
        if ($process.HasExited) { throw "Runtime exited during title cycle (code $($process.ExitCode))." }
        if ($step -gt 0) {
            if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
                [void][ManiacTitleCycleKeys]::SetForegroundWindow($process.MainWindowHandle)
                [ManiacTitleCycleKeys]::keybd_event(13, 0, 0, [UIntPtr]::Zero)
                Start-Sleep -Milliseconds 80
                [ManiacTitleCycleKeys]::keybd_event(13, 0, 2, [UIntPtr]::Zero)
            }
            Start-Sleep -Milliseconds 500
        }
        foreach ($slot in 1, 2) {
            $save = Join-Path $fixturePath ('Save{0:D2}.lsd' -f $slot)
            if (-not (Test-Path -LiteralPath $save)) { continue }
            $file = Get-Item -LiteralPath $save
            $signature = "$($file.LastWriteTimeUtc.Ticks):$($file.Length)"
            if ($observedTimes[$slot] -eq $signature) { continue }
            $observedTimes[$slot] = $signature
            $snapshotName = ('event{0:D2}-slot{1}.lsd' -f ($events.Count + 1), $slot)
            $snapshot = Join-Path $outputPath $snapshotName
            Copy-Item -LiteralPath $save -Destination $snapshot -Force
            $events.Add([ordered]@{
                input_enter_count = $step
                save_slot = $slot
                snapshot = $snapshotName
                sha256 = (Get-FileHash -LiteralPath $snapshot -Algorithm SHA256).Hash.ToLowerInvariant()
                length = (Get-Item -LiteralPath $snapshot).Length
            })
        }
        if ($events.Count -ge 4) {
            $lastSlots = @($events | Select-Object -Last 4 | ForEach-Object { $_.save_slot })
            if (($lastSlots -join ',') -eq '1,2,1,2') { break }
        }
    }
    $result = [ordered]@{
        reference_sha256 = $manifest.files.'RPG_RT.exe'
        fixture_manifest_sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
        runtime_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
        runtime_arguments = $RuntimeArguments
        events = $events
    }
    $json = $result | ConvertTo-Json -Depth 6
    $json | Set-Content -LiteralPath (Join-Path $outputPath 'trace.json') -Encoding utf8
    if ($events.Count -lt 4 -or ((@($events | Select-Object -Last 4 | ForEach-Object { $_.save_slot })) -join ',') -ne '1,2,1,2') {
        throw "Two title/new-game save cycles were not observed; trace saved in $outputPath."
    }
    Write-Output $json
} finally {
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
