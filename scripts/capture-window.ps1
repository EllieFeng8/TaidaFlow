# Evidence helper: save a PNG of a top-level window (PrintWindow, works when occluded).
# Usage: powershell -ExecutionPolicy Bypass -File scripts\capture-window.ps1 -ProcessName TaidaFlowApp -Out docs\evidence\wasm-v4\x.png
param(
    [Parameter(Mandatory = $true)][string]$ProcessName,
    [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class TfWin {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[void][TfWin]::SetProcessDPIAware()
$p = Get-Process -Name $ProcessName | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { Write-Error "no window for $ProcessName"; exit 2 }
$r = New-Object TfWin+RECT
[void][TfWin]::GetWindowRect($p.MainWindowHandle, [ref]$r)
$w = $r.R - $r.L; $h = $r.B - $r.T
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [TfWin]::PrintWindow($p.MainWindowHandle, $hdc, 2)   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()
$dir = Split-Path -Parent $Out
if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
if (-not $ok) { Write-Error "PrintWindow failed"; exit 3 }
Write-Output "saved $Out (${w}x${h})"
exit 0
