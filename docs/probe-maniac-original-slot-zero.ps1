param(
    [string]$Fixture = (Join-Path $PSScriptRoot '..\build\maniac-original-fixture'),
    [string]$Output = (Join-Path $PSScriptRoot '..\build\maniac-original-slot-zero-trace.json'),
    [string]$ExpectedInputSaveSha256 = '',
    [int]$MinimumSamples = 0
)

$ErrorActionPreference = 'Stop'
$fixturePath = (Resolve-Path -LiteralPath $Fixture).Path
$exe = Join-Path $fixturePath 'RPG_RT.exe'
$expectedHash = '082df5cf00fd3c7647db52d400b53bb9db2350dd71d672709f59bc537a67fb3f'
$actualHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $expectedHash) {
    throw "Unexpected RPG_RT.exe hash: $actualHash"
}
$manifestPath = Join-Path $fixturePath 'manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$expectedSavePath = $null
if ($null -ne $manifest.expected_save_slot) {
    $expectedSavePath = Join-Path $fixturePath ('Save{0:D2}.lsd' -f [int]$manifest.expected_save_slot)
}
$expectedSlots = @($manifest.expected_global_slots_after_event)
if ($expectedSlots.Count -eq 0) { throw 'Fixture manifest has no expected global slots.' }
$expectedSwitchSlots = @($manifest.expected_global_switch_slots_after_event)
if ($expectedSwitchSlots.Count -eq 0) { throw 'Fixture manifest has no expected global switch slots.' }
foreach ($file in $manifest.files.PSObject.Properties) {
    $path = Join-Path $fixturePath $file.Name
    $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $file.Value) {
        throw "Fixture input changed: $($file.Name)"
    }
}
if ($expectedSavePath -and (Test-Path -LiteralPath $expectedSavePath)) {
    Remove-Item -LiteralPath $expectedSavePath
}
$inputSaveSha256 = $null
if ($ExpectedInputSaveSha256) {
    $inputSave = Join-Path $fixturePath 'Save01.lsd'
    $inputSaveSha256 = (Get-FileHash -LiteralPath $inputSave -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($inputSaveSha256 -ne $ExpectedInputSaveSha256.ToLowerInvariant()) {
        throw "Unexpected input save hash: $inputSaveSha256"
    }
}

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ManiacProbeNative {
    [DllImport("kernel32.dll")] public static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
    [DllImport("kernel32.dll")] public static extern bool ReadProcessMemory(IntPtr process, IntPtr address, byte[] buffer, int size, out IntPtr read);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern void keybd_event(byte key, byte scan, uint flags, UIntPtr extra);
}
'@

$process = Start-Process -FilePath $exe -WorkingDirectory $fixturePath -WindowStyle Hidden -PassThru
$handle = [IntPtr]::Zero
try {
    Start-Sleep -Seconds 2
    $process.Refresh()
    if ($process.HasExited -or $process.MainWindowHandle -eq [IntPtr]::Zero) {
        throw 'Original runtime exited or did not create its game window.'
    }
    $handle = [ManiacProbeNative]::OpenProcess(0x410, $false, $process.Id)
    if ($handle -eq [IntPtr]::Zero) {
        throw 'Cannot read the original runtime process.'
    }
    $base = $process.MainModule.BaseAddress.ToInt64()
    $samples = @()
    $everMatched = $false
    $baseMaxSteps = if ($expectedSavePath) { 16 } else { 8 }
    $maxSteps = [Math]::Max($baseMaxSteps, $MinimumSamples - 1)
    for ($step = 0; $step -le $maxSteps; $step++) {
        if ($step -gt 0) {
            [void][ManiacProbeNative]::SetForegroundWindow($process.MainWindowHandle)
            [ManiacProbeNative]::keybd_event(13, 0, 0, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 80
            [ManiacProbeNative]::keybd_event(13, 0, 2, [UIntPtr]::Zero)
            Start-Sleep -Milliseconds 500
        }
        $pointers = New-Object byte[] 16
        $read = [IntPtr]::Zero
        if (-not [ManiacProbeNative]::ReadProcessMemory($handle, [IntPtr]($base + 0x27a4b8), $pointers, 16, [ref]$read)) {
            throw 'Cannot read the original global numeric vector pointers.'
        }
        $begin = [BitConverter]::ToInt64($pointers, 0)
        $end = [BitConverter]::ToInt64($pointers, 8)
        $length = [int](($end - $begin) / 4)
        if ($begin -le 0 -or $length -lt 0 -or $length -gt 100000) {
            throw "Invalid original global numeric vector: begin=$begin end=$end"
        }
        $values = New-Object byte[] ([Math]::Max(4, [Math]::Min($length, $expectedSlots.Count) * 4))
        if (-not [ManiacProbeNative]::ReadProcessMemory($handle, [IntPtr]$begin, $values, $values.Length, [ref]$read)) {
            throw 'Cannot read the original global numeric vector values.'
        }
        $slots = @()
        for ($index = 0; $index -lt $expectedSlots.Count; $index++) {
            if ($index -lt $length) { $slots += [BitConverter]::ToInt32($values, $index * 4) }
            else { $slots += $null }
        }
        $switchPointers = New-Object byte[] 16
        if (-not [ManiacProbeNative]::ReadProcessMemory($handle, [IntPtr]($base + 0x27a4a0), $switchPointers, 16, [ref]$read)) {
            throw 'Cannot read the original global switch vector pointers.'
        }
        $switchBegin = [BitConverter]::ToInt64($switchPointers, 0)
        $switchEnd = [BitConverter]::ToInt64($switchPointers, 8)
        $switchLength = [int]($switchEnd - $switchBegin)
        if ($switchBegin -le 0 -or $switchLength -lt 0 -or $switchLength -gt 100000) {
            throw "Invalid original global switch vector: begin=$switchBegin end=$switchEnd"
        }
        $rawSwitch = New-Object byte[] ([Math]::Max(1, [Math]::Min($switchLength, $expectedSwitchSlots.Count)))
        if ($switchLength -gt 0 -and -not [ManiacProbeNative]::ReadProcessMemory($handle, [IntPtr]$switchBegin, $rawSwitch, $rawSwitch.Length, [ref]$read)) {
            throw 'Cannot read the original global switch slots.'
        }
        $switchSlots = @()
        for ($index = 0; $index -lt $expectedSwitchSlots.Count; $index++) {
            if ($index -lt $switchLength) { $switchSlots += [int]$rawSwitch[$index] }
            else { $switchSlots += $null }
        }
        $sample = [ordered]@{
            input_enter_count = $step
            vector_length = $length
            slots = $slots
            global_switch_vector_length = $switchLength
            global_switch_slots_raw = $switchSlots
        }
        $samples += $sample
        if ($step -eq 0 -and -not $manifest.save_probe -and ($length -ne 1 -or $slots[0] -ne 0)) {
            throw 'Unexpected original initial numeric vector before input.'
        }
        $matched = $true
        for ($index = 0; $index -lt $expectedSlots.Count; $index++) {
            if ($slots[$index] -ne [int]$expectedSlots[$index]) { $matched = $false; break }
        }
        if ($matched) {
            for ($index = 0; $index -lt $expectedSwitchSlots.Count; $index++) {
                if ($switchSlots[$index] -ne [int]$expectedSwitchSlots[$index]) { $matched = $false; break }
            }
        }
        if ($matched -and ($length -ne [int]$manifest.expected_global_numeric_vector_length_after_event -or
            $switchLength -ne [int]$manifest.expected_global_switch_vector_length_after_event)) {
            $matched = $false
        }
        if ($matched -and $expectedSavePath -and -not (Test-Path -LiteralPath $expectedSavePath)) {
            $matched = $false
        }
        if ($matched) { $everMatched = $true }
        if ($matched -and $samples.Count -ge $MinimumSamples) { break }
    }
    $result = [ordered]@{
        reference_sha256 = $actualHash
        fixture_manifest_sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
        observation = 'RVA 0x27a4b8 global numeric vector begin/end; startup advances with Enter'
        expected_slots = $expectedSlots
        samples = $samples
    }
    if ($expectedSavePath -and (Test-Path -LiteralPath $expectedSavePath)) {
        $result.save_file_sha256 = (Get-FileHash -LiteralPath $expectedSavePath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    if ($inputSaveSha256) { $result.input_save_sha256 = $inputSaveSha256 }
    $json = $result | ConvertTo-Json -Depth 6
    $json | Set-Content -LiteralPath $Output -Encoding utf8
    Write-Output $json
    if (-not $everMatched) {
        throw 'The original event did not reach the expected slot values.'
    }
} finally {
    if ($handle -ne [IntPtr]::Zero) { [void][ManiacProbeNative]::CloseHandle($handle) }
    $process.Refresh()
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
