param(
    [string] $Out = 'C:\Users\Administrator\Downloads\NHL.27.PS5-DUPLEX\screen.png',
    [int] $Width = 960
)
# Captures the emulator window without focusing it (PrintWindow with PW_RENDERFULLCONTENT, which
# also works while the window is covered). Scales to $Width pixels wide; 0 keeps the full size.
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class KytyCap {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
}
'@
$proc = Get-Process kyty_emulator -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
if (-not $proc) { throw 'no kyty_emulator window' }
$r = New-Object KytyCap+RECT
[void][KytyCap]::GetClientRect($proc.MainWindowHandle, [ref]$r)
$w = $r.R - $r.L; $h = $r.B - $r.T
if ($w -le 0 -or $h -le 0) { throw "window has no client area ($w x $h); minimized?" }
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$dc = $g.GetHdc()
# 1 = PW_CLIENTONLY, 2 = PW_RENDERFULLCONTENT
$ok = [KytyCap]::PrintWindow($proc.MainWindowHandle, $dc, 3)
$g.ReleaseHdc($dc); $g.Dispose()
if ($Width -gt 0 -and $Width -lt $w) {
    $sh = [int]($h * $Width / $w)
    $small = New-Object System.Drawing.Bitmap $Width, $sh
    $g2 = [System.Drawing.Graphics]::FromImage($small)
    $g2.InterpolationMode = 'HighQualityBilinear'
    $g2.DrawImage($bmp, 0, 0, $Width, $sh); $g2.Dispose(); $bmp.Dispose(); $bmp = $small
}
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
"captured=$ok size=${w}x${h} out=$Out"
