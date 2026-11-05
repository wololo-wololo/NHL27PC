# Shared helpers: capture the emulator window without focusing it and compute a signature of the
# button-prompt bar at the bottom of NHL 27's menus (static text, unaffected by animated
# backgrounds). Dot-source this file.
Add-Type -AssemblyName System.Drawing
if (-not ('KytyScreen' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class KytyScreen {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);

    public static Bitmap Capture(IntPtr hwnd) {
        RECT r; GetClientRect(hwnd, out r);
        int w = r.R - r.L, h = r.B - r.T;
        if (w <= 0 || h <= 0) return null;
        var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        using (var g = Graphics.FromImage(bmp)) {
            IntPtr dc = g.GetHdc();
            PrintWindow(hwnd, dc, 3); // PW_CLIENTONLY | PW_RENDERFULLCONTENT
            g.ReleaseHdc(dc);
        }
        return bmp;
    }

    // Bright-pixel mask of the bottom prompt bar, Cols x Rows cells, as a bit string.
    public const int Cols = 120, Rows = 6;
    public static string Signature(Bitmap bmp) {
        int w = bmp.Width, h = bmp.Height;
        int x0 = (int)(w * 0.10), x1 = (int)(w * 0.90);
        int y0 = (int)(h * 0.935), y1 = (int)(h * 0.985);
        var data = bmp.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        var px = new int[w * h];
        Marshal.Copy(data.Scan0, px, 0, px.Length);
        bmp.UnlockBits(data);
        var bits = new char[Cols * Rows];
        for (int cy = 0; cy < Rows; cy++)
        for (int cx = 0; cx < Cols; cx++) {
            int ax = x0 + (x1 - x0) * cx / Cols, bx = x0 + (x1 - x0) * (cx + 1) / Cols;
            int ay = y0 + (y1 - y0) * cy / Rows, by = y0 + (y1 - y0) * (cy + 1) / Rows;
            int bright = 0, total = 0;
            for (int y = ay; y < by; y++)
            for (int x = ax; x < bx; x++) {
                int p = px[y * w + x];
                int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
                // Prompt text is near-white; backgrounds rarely are.
                if (r > 200 && g > 200 && b > 200) bright++;
                total++;
            }
            bits[cy * Cols + cx] = bright * 12 >= total ? '1' : '0';
        }
        return new string(bits);
    }

    // Fraction of cells that differ, normalised by the cells lit in either signature.
    public static double Distance(string a, string b) {
        int diff = 0, lit = 0;
        for (int i = 0; i < a.Length; i++) {
            bool x = a[i] == '1', y = b[i] == '1';
            if (x != y) diff++;
            if (x || y) lit++;
        }
        return lit == 0 ? 1.0 : (double)diff / lit;
    }
}
'@
}

function Get-KytyWindow {
    $p = Get-Process kyty_emulator -ErrorAction SilentlyContinue | Where-Object MainWindowHandle -ne 0 | Select-Object -First 1
    if ($p) { $p.MainWindowHandle } else { [IntPtr]::Zero }
}

function Save-KytyFrame([System.Drawing.Bitmap] $Bitmap, [string] $Path, [int] $Width = 960) {
    if ($Width -gt 0 -and $Width -lt $Bitmap.Width) {
        $h = [int]($Bitmap.Height * $Width / $Bitmap.Width)
        $small = New-Object System.Drawing.Bitmap $Width, $h
        $g = [System.Drawing.Graphics]::FromImage($small)
        $g.InterpolationMode = 'HighQualityBilinear'
        $g.DrawImage($Bitmap, 0, 0, $Width, $h); $g.Dispose()
        $small.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png); $small.Dispose()
    } else {
        $Bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    }
}
