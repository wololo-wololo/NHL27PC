param([Parameter(Mandatory)] [string] $Name, [int] $Seconds = 400, [switch] $Kill)
# Watches a running measurement (perf-<Name>.log). When PM4 has been idle for 8 s after gameplay
# activity, attaches lldb to the emulator and writes every thread's stack to
# hang-threads-<Name>.txt in the workspace root. Run it in parallel with measure.ps1.
$root = 'C:\Users\Administrator\Downloads\NHL.27.PS5-DUPLEX'
$log  = Join-Path $root "perf-$Name.log"
Start-Sleep -Seconds 20
$result = & (Join-Path $PSScriptRoot 'waithang.ps1') -Log $log -Seconds $Seconds
"hangwatch: $result"
if ($result -like 'hang at*') {
    $emu = Get-Process kyty_emulator -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($emu) {
        $out = Join-Path $root "hang-threads-$Name.txt"
        & 'C:\Program Files\LLVM\bin\lldb.exe' -p $emu.Id --batch -o 'thread backtrace all -c 64' -o 'detach' *> $out
        "hangwatch: stacks written to $out"
        if ($Kill) { Stop-Process -Id $emu.Id -Force -ErrorAction SilentlyContinue }
    }
}
