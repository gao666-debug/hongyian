Add-Type @'
using System;
using System.Runtime.InteropServices;
public class CareWindow { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int n); }
'@
$p = Get-Process devecostudio64 | Where-Object {$_.MainWindowHandle -ne 0} | Select-Object -First 1
if ($p) { [CareWindow]::ShowWindow($p.MainWindowHandle,9) | Out-Null; [CareWindow]::SetForegroundWindow($p.MainWindowHandle) | Out-Null }
