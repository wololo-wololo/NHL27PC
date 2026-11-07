param(
    [Parameter(Mandatory)] [string] $Name,
    [string] $Sample = '45,25',
    [int] $Seconds = 75,
    [string] $Env = '',
    # Drive the menus to Play Now CPU-vs-CPU gameplay (autopilot.ps1); -Seconds then counts from
    # gameplay start instead of launch.
    [switch] $Autopilot,
    [int] $AutopilotTimeout = 420,
    # With -Autopilot: emulator-side screenshots this many seconds after gameplay starts, saved as
    # autopilot-shots\<Name>\gameplay+<s>.png (rendering checks).
    [int[]] $ShotsAfter = @(),
    # With -Autopilot: tap Cross this many times (1.5 s apart) once the arena renders, to skip the
    # pre-game presentation to the opening faceoff. Gameplay time then starts after the taps.
    [int] $SkipIntro = 0
)
# One measured run: stops any running emulator, stages the current build, runs NHL 27 with
# the perf log and GPU-thread sampler, and closes the emulator after $Seconds (or a crash).
# Environment variables set in the calling shell are inherited by the emulator, which avoids the
# quoting problems of -Env.
$root = 'C:\Users\Administrator\Downloads\NHL.27.PS5-DUPLEX'
$kyty = Join-Path $root 'KytyPS5-2026-11-01-telemetry-Windows-x64'
Get-Process kyty_emulator -ErrorAction SilentlyContinue | Stop-Process -Force -PassThru | Wait-Process -Timeout 30 -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
Copy-Item "$root\nhl27pc\_Build\windows-local\kyty_emulator.exe", "$root\nhl27pc\_Build\windows-local\kyty_emulator.pdb" $kyty -Force -ErrorAction Stop
Remove-Item "$kyty\thread-samples.txt", "$root\perf-$Name.log" -ErrorAction SilentlyContinue
$command = "Set-Location '$root\nhl27pc'; `$env:KYTY_SAMPLE_GPU='$Sample'; $Env " +
    ".\tools\run_nhl27.ps1 -GameDirectory '..\PPSA34063-app0' -KytyDirectory '..\KytyPS5-2026-11-01-telemetry-Windows-x64' " +
    "-Optimized -PerfLog '..\perf-$Name.log' -EmulatorArguments '--perf-overlay' *> '$root\console-$Name.log'"
if ($Autopilot) { $env:KYTY_INPUT_PIPE = '1' }
$launched = Get-Date
Start-Process powershell -WindowStyle Hidden -ArgumentList '-NoProfile', '-Command', $command
# Every run is watched for a PM4 hang; on one, hangwatch.ps1 dumps all thread stacks with lldb
# (hang-threads-<Name>.txt) and closes the emulator.
$watch = Start-Job -ArgumentList $PSScriptRoot, $Name -ScriptBlock {
    param($dir, $name) & (Join-Path $dir 'hangwatch.ps1') -Name $name -Seconds 1200 -Kill
}
Start-Sleep -Seconds 5
$gameplayAt = ''
if ($Autopilot) {
    $shots = Join-Path $root "autopilot-shots\$Name"
    & (Join-Path $PSScriptRoot 'autopilot.ps1') -PerfLog "$root\perf-$Name.log" `
        -TimeoutSeconds $AutopilotTimeout -ShotDir $shots | Out-Host
    if ($LASTEXITCODE -ne 0) {
        Get-Process kyty_emulator -ErrorAction SilentlyContinue | Stop-Process -Force -PassThru | Wait-Process -Timeout 30 -ErrorAction SilentlyContinue
        $hw = (Receive-Job $watch -Wait -AutoRemoveJob | Out-String).Trim() -replace "`r?`n", '; '
        "run=$Name autopilot_failed=$LASTEXITCODE shots=$shots $hw"
        exit 1
    }
    for ($i = 0; $i -lt $SkipIntro; $i++) {
        & (Join-Path $PSScriptRoot 'pad.ps1') 'tap cross' *> $null
        Start-Sleep -Milliseconds 1500
    }
    $gameplayAt = (Get-Content "$root\perf-$Name.log" -Tail 1) -replace '^t=([0-9.]+).*', '$1'
    $elapsed = [int]((Get-Date) - $launched).TotalSeconds
    "gameplay $elapsed s after launch (perf t=$gameplayAt)" | Out-Host
}
$deadline = (Get-Date).AddSeconds($Seconds)
if ($Autopilot -and $ShotsAfter.Count -gt 0) {
    . (Join-Path $PSScriptRoot 'screen.ps1')
    $gameplayStart = Get-Date
    foreach ($s in ($ShotsAfter | Sort-Object)) {
        while (((Get-Date) - $gameplayStart).TotalSeconds -lt $s -and
               (Get-Process kyty_emulator -ErrorAction SilentlyContinue)) { Start-Sleep -Milliseconds 500 }
        $bmp = Join-Path $env:TEMP "measure-$Name-$s.bmp"
        Remove-Item $bmp -ErrorAction SilentlyContinue
        foreach ($try in 1..5) {
            & (Join-Path $PSScriptRoot 'pad.ps1') "shot $bmp" *> $null
            if (Test-Path $bmp) { break }
            Start-Sleep -Seconds 1
        }
        if (Test-Path $bmp) {
            $b = New-Object System.Drawing.Bitmap $bmp
            Save-KytyFrame $b (Join-Path $root "autopilot-shots\$Name\gameplay+$s.png") 960
            $b.Dispose()
            "screenshot gameplay+$s s saved" | Out-Host
        }
    }
}
while ((Get-Date) -lt $deadline -and (Get-Process kyty_emulator -ErrorAction SilentlyContinue)) { Start-Sleep -Seconds 2 }
$crashed = -not (Get-Process kyty_emulator -ErrorAction SilentlyContinue)
Get-Process kyty_emulator -ErrorAction SilentlyContinue | Stop-Process -Force -PassThru | Wait-Process -Timeout 30 -ErrorAction SilentlyContinue
if (Test-Path "$kyty\thread-samples.txt") { Copy-Item "$kyty\thread-samples.txt" "$root\thread-samples-$Name.txt" -Force }
$hw = (Receive-Job $watch -Wait -AutoRemoveJob | Out-String).Trim() -replace "`r?`n", '; '
"run=$Name crashed_early=$crashed gameplay_t=$gameplayAt samples=$(Test-Path "$root\thread-samples-$Name.txt") $hw"
