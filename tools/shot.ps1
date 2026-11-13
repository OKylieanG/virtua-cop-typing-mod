param([string]$Out)
# DPI-aware screenshot of the game's client area (no window activation, so the game keeps focus)
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class GW {
  [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rt,B; }
  [StructLayout(LayoutKind.Sequential)] public struct P { public int X,Y; }
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out R r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref P p);
}
'@
[GW]::SetProcessDPIAware() | Out-Null
$h = (Get-Process PPJDD).MainWindowHandle
$r = New-Object GW+R; [GW]::GetClientRect($h, [ref]$r) | Out-Null
$p = New-Object GW+P; [GW]::ClientToScreen($h, [ref]$p) | Out-Null
$bmp = New-Object Drawing.Bitmap $r.Rt, $r.B
$g = [Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($p.X, $p.Y, 0, 0, $bmp.Size)
$bmp.Save($Out)
"client $($r.Rt)x$($r.B) at $($p.X),$($p.Y)"
