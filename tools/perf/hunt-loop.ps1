param(
    # Prefix for run names; logs land in the workspace root as console-<Prefix>-<n>.log.
    [string] $Prefix = 'hunt',
    [int] $Attempts = 8,
    # Stop after this many captured device losses.
    [int] $Captures = 2,
    [int] $Seconds = 150
)
# Device-loss reproduction loop with the diagnostics that localize NHL 27's GPU hang
# (see docs/NHL27-60FPS-PROGRESS.md "Resume here"). Each attempt is an autopilot run; the
# report after a device loss is in console-<name>.log ("GPU buffer markers after device loss").
Set-Location (Join-Path $PSScriptRoot '..\..')
$env:KYTY_GPU_CHECKPOINTS = '1'
$env:KYTY_GPU_CHECKPOINTS_MASK = '15'
$env:KYTY_GPU_CHECKPOINTS_TOP_ONLY = '1'
$env:KYTY_GPU_CHECKPOINTS_BOTTOM_EVERY = '8'
$env:KYTY_GPU_LOG_INDIRECT = '1'
$found = 0
foreach ($n in 1..$Attempts) {
    $name = "$Prefix-$n"
    $o = & (Join-Path $PSScriptRoot 'measure.ps1') -Name $name -Autopilot -Seconds $Seconds -Sample '0,0'
    "attempt $n : $($o[-1])"
    if (Select-String -Path "..\console-$name.log" -Pattern 'buffer markers after device loss' -Quiet) {
        "device loss captured in $name"
        $found++
        if ($found -ge $Captures) { break }
    }
    Start-Sleep -Seconds 5
}
