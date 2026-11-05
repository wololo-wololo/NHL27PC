param(
    [double] $Seconds = 10,
    [int] $Top = 14
)
# Per-thread CPU use of the running emulator over a window: shows whether a guest (game) thread
# is saturated while the GPU thread waits for work. Thread ids only; the GPU thread is the
# emulator's busiest host thread during gameplay.
$p = Get-Process kyty_emulator -ErrorAction Stop
$before = @{}
foreach ($t in $p.Threads) { try { $before[$t.Id] = $t.TotalProcessorTime.TotalMilliseconds } catch {} }
Start-Sleep -Milliseconds ([int]($Seconds * 1000))
$p.Refresh()
$rows = foreach ($t in $p.Threads) {
    try {
        $now = $t.TotalProcessorTime.TotalMilliseconds
        if ($before.ContainsKey($t.Id)) {
            [pscustomobject]@{
                Id       = $t.Id
                CpuPct   = [math]::Round(100.0 * ($now - $before[$t.Id]) / ($Seconds * 1000), 1)
                Priority = $t.PriorityLevel
                State    = $t.ThreadState
            }
        }
    } catch {}
}
$total = ($rows | Measure-Object CpuPct -Sum).Sum
"process CPU: {0:N0}% of one core over {1} s, {2} threads" -f $total, $Seconds, $rows.Count
$rows | Sort-Object CpuPct -Descending | Select-Object -First $Top | Format-Table -AutoSize
