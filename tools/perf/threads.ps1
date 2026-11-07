param(
    [double] $Seconds = 10,
    [int] $Top = 14
)
# Per-thread CPU use of the running emulator over a window: shows whether a guest (game) thread
# is saturated while the GPU thread waits for work. Names come from the Windows thread
# description: guest threads are "guest:<name>", the GPU thread "emu:Thread_Gpu"; other emulator
# threads (recorder worker, priority operations, presenter, driver) are unnamed.
Add-Type -Namespace KytyPerf -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", SetLastError = true)]
public static extern System.IntPtr OpenThread(uint access, bool inherit, uint id);
[DllImport("kernel32.dll")]
public static extern bool CloseHandle(System.IntPtr handle);
[DllImport("kernel32.dll")]
public static extern int GetThreadDescription(System.IntPtr thread, out System.IntPtr description);
[DllImport("kernel32.dll")]
public static extern System.IntPtr LocalFree(System.IntPtr memory);
public static string Name(uint id) {
    System.IntPtr handle = OpenThread(0x1000, false, id); // THREAD_QUERY_LIMITED_INFORMATION
    if (handle == System.IntPtr.Zero) return "";
    string result = "";
    System.IntPtr text;
    if (GetThreadDescription(handle, out text) >= 0 && text != System.IntPtr.Zero) {
        result = System.Runtime.InteropServices.Marshal.PtrToStringUni(text);
        LocalFree(text);
    }
    CloseHandle(handle);
    return result;
}
'@ -ErrorAction SilentlyContinue
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
                Id     = $t.Id
                CpuPct = [math]::Round(100.0 * ($now - $before[$t.Id]) / ($Seconds * 1000), 1)
                Name   = [KytyPerf.Native]::Name([uint32]$t.Id)
                State  = $t.ThreadState
            }
        }
    } catch {}
}
$total = ($rows | Measure-Object CpuPct -Sum).Sum
"process CPU: {0:N0}% of one core over {1} s, {2} threads" -f $total, $Seconds, $rows.Count
$rows | Sort-Object CpuPct -Descending | Select-Object -First $Top | Format-Table -AutoSize
