param([string]$Log, [int]$Seconds = 560)
# Returns when the emulator exits, or when PM4 has been idle for 8 consecutive seconds after
# gameplay activity (a fatal hang rather than a load gap or hitch).
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
    if (-not (Get-Process kyty_emulator -ErrorAction SilentlyContinue)) { "exited"; exit }
    if (Test-Path $Log) {
        $lines = @(Get-Content $Log | Where-Object { $_ -like 't=*' })
        if ($lines.Count -gt 40) {
            $tail = $lines[-8..-1]
            $idle = @($tail | Where-Object { $_ -notmatch 'Pm4ProcessCalls=' }).Count
            if ($idle -eq 8) { "hang at " + ($tail[0] -split ' ')[0]; exit }
        }
    }
    Start-Sleep -Seconds 4
}
"no hang"
