param(
  [string]$ProcessName = "VulkanEngineGame",
  [string]$Out = "C:\Users\fahre\Desktop\vc_capture.png"
)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$src = @"
using System;
using System.Runtime.InteropServices;
[StructLayout(LayoutKind.Sequential)]
public struct RECT { public int Left, Top, Right, Bottom; }
public class PW {
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, ref RECT rect);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
}
"@
Add-Type -TypeDefinition $src
$p = Get-Process $ProcessName -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 }
if (-not $p) {
  # fallback: qualquer processo com janela com esse nome
  $p = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
}
if (-not $p) { Write-Output "NO-PROC"; exit 1 }
$hwnd = $p.MainWindowHandle
if ($hwnd -eq 0) { Write-Output "HWND-0"; exit 1 }
[PW]::SetForegroundWindow($hwnd) | Out-Null
Start-Sleep -Milliseconds 400
$r = New-Object RECT
[PW]::GetWindowRect($hwnd, [ref]$r) | Out-Null
$w = $r.Right - $r.Left; $h = $r.Bottom - $r.Top
if ($w -le 0 -or $h -le 0) { Write-Output "BAD-RECT $w x $h"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap $w, $h
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[PW]::PrintWindow($hwnd, $hdc, 2) | Out-Null
$g.ReleaseHdc($hdc)
$g.Dispose()
$bmp.Save($Out)
Write-Output ("WIN " + $w + "x" + $h + " hwnd=" + $hwnd + " saved=" + (Test-Path $Out))