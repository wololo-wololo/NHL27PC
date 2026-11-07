param([string] $Prefix = 'hrep', [int] $Runs = 5, [int] $Seconds = 90, [int] $SkipIntro = 8,
      [switch] $StopOnHang)
# Repeats measure.ps1 -Autopilot (environment inherited from the caller, e.g.
# KYTY_ASYNC_RECORD=0). measure.ps1 watches each run; a hang's thread stacks land in
# hang-threads-<Prefix>-<n>.txt in the workspace root. Prints one line per run and the totals.
$root = 'C:\Users\Administrator\Downloads\NHL.27.PS5-DUPLEX'
$hangs = 0; $losses = 0
for ($n = 1; $n -le $Runs; $n++) {
    $name = "$Prefix-$n"
    $result = & (Join-Path $PSScriptRoot 'measure.ps1') -Name $name -Seconds $Seconds -Autopilot `
        -SkipIntro $SkipIntro -Sample '0,0' -AutopilotTimeout 300 *>&1 | Select-Object -Last 1
    $lost = Select-String -Path (Join-Path $root "console-$name.log") -Pattern 'masterSemaphore.cpp:9' -Quiet
    $hung = Test-Path (Join-Path $root "hang-threads-$name.txt")
    if ($lost) { $losses++ }
    if ($hung) { $hangs++ }
    "$name : device_lost=$lost hang=$hung | $result"
    if ($hung -and $StopOnHang) { break }
}
"total: $Runs runs, $losses device losses, $hangs hangs"
