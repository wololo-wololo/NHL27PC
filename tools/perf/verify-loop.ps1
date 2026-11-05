param(
    [string] $Prefix = 'verify',
    [int] $Runs = 10,
    [int] $Seconds = 150
)
# Fix verification: default configuration (default markers only), N autopilot runs; prints
# per run whether the device was lost. Baseline before the hang fixes: ~1 in 3 runs lost it.
Set-Location (Join-Path $PSScriptRoot '..\..')
$lost = 0
foreach ($n in 1..$Runs) {
    $name = "$Prefix-$n"
    $o = & (Join-Path $PSScriptRoot 'measure.ps1') -Name $name -Autopilot -Seconds $Seconds -Sample '0,0'
    $dead = Select-String -Path "..\console-$name.log" -Pattern 'buffer markers after device loss|masterSemaphore' -Quiet
    $last = (Get-Content "..\perf-$name.log" -Tail 1) -replace '^t=([0-9.]+).*', '$1'
    if ($dead) { $lost++ }
    "run $n : $($o[-1]) device_loss=$dead last_t=$last"
    Start-Sleep -Seconds 5
}
"device losses: $lost / $Runs"
