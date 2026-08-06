# Launches two Fallout 2 co-op instances side by side, each with its own
# console/debug log. Called from launch_two_instances.bat.
$ErrorActionPreference = 'Stop'

# Save slots to auto-load: instance 1 hosts with $HostSlot, instance 2
# joins with $ClientSlot. Slots are 1-based (1 = SLOT01). Both slots must
# hold valid, non-corrupt saves.
$HostSlot = 1
$ClientSlot = 2

$dist = Join-Path $PSScriptRoot 'fallout2coopdist'
$exe = Join-Path $dist 'fallout2-ce.exe'
$log1 = Join-Path $dist 'coop_instance1.log'
$log2 = Join-Path $dist 'coop_instance2.log'

if (-not (Test-Path -LiteralPath $exe)) {
    Write-Host "Missing executable: $exe"
    exit 1
}

Add-Type -AssemblyName System.Windows.Forms

Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class CoopWin {
    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
}
"@

# Auto co-op session: instance 1 loads its save slot and hosts, instance 2
# loads its save slot and joins 127.0.0.1.
$arg1 = "[debug]console_output_path=`"$log1`" --dev-load-game=$HostSlot --coop-host"
$arg2 = "[debug]console_output_path=`"$log2`" --dev-load-game=$ClientSlot --coop-join=127.0.0.1"

$p1 = Start-Process -FilePath $exe -WorkingDirectory $dist -ArgumentList $arg1 -PassThru
$p2 = Start-Process -FilePath $exe -WorkingDirectory $dist -ArgumentList $arg2 -PassThru

# Wait for both main windows to appear (they are created during startup).
foreach ($p in @($p1, $p2)) {
    $deadline = (Get-Date).AddSeconds(30)
    while ($p.MainWindowHandle -eq 0 -and -not $p.HasExited -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 200
        $p.Refresh()
    }
}

if ($p1.HasExited -or $p2.HasExited) {
    Write-Host "An instance exited before its window appeared."
    exit 1
}

$work = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea

$rect = New-Object CoopWin+RECT
[CoopWin]::GetWindowRect($p1.MainWindowHandle, [ref]$rect) | Out-Null
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top

$margin = 8
$targetWidth = [Math]::Min($width, [int](($work.Width - $margin * 3) / 2))
$targetHeight = [Math]::Min($height, $work.Height - $margin * 2)

# SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOZORDER
$flags = 0x0040 -bor 0x0010 -bor 0x0004

[CoopWin]::SetWindowPos($p1.MainWindowHandle, [IntPtr]::Zero,
    $work.Left + $margin, $work.Top + $margin,
    $targetWidth, $targetHeight, $flags) | Out-Null
[CoopWin]::SetWindowPos($p2.MainWindowHandle, [IntPtr]::Zero,
    $work.Left + $margin * 2 + $targetWidth, $work.Top + $margin,
    $targetWidth, $targetHeight, $flags) | Out-Null

Write-Host "Started two Fallout 2 instances side by side."
Write-Host "Instance 1 log: $log1"
Write-Host "Instance 2 log: $log2"

# Poll both logs until the co-op session is up or 30s elapse. The session is
# considered connected when the host announces success and the client receives
# the welcome and completes its initial map sync.
$deadline = (Get-Date).AddSeconds(30)
$hostUp = $false
$clientUp = $false
while ((Get-Date) -lt $deadline -and -not ($hostUp -and $clientUp)) {
    if (-not $hostUp -and (Test-Path -LiteralPath $log1)) {
        $hostUp = Select-String -Path $log1 -Pattern "MP: MpHostStart success" -Quiet
    }
    if (-not $clientUp -and (Test-Path -LiteralPath $log2)) {
        $clientUp = Select-String -Path $log2 -Pattern "MP: welcome received" -Quiet
    }
    if (-not ($hostUp -and $clientUp)) {
        Start-Sleep -Milliseconds 500
    }
}

if ($hostUp) {
    Write-Host "Instance 1 (Host): CONNECTED"
} else {
    Write-Host "Instance 1 (Host): NOT CONNECTED (no 'MP: MpHostStart success' in log)"
}
if ($clientUp) {
    Write-Host "Instance 2 (Client): CONNECTED"
} else {
    Write-Host "Instance 2 (Client): NOT CONNECTED (no 'MP: welcome received' in log)"
}

# Keep the console open (status readable) until both instances are closed,
# then exit so the bat window closes with them.
Wait-Process -Id $p1.Id, $p2.Id -ErrorAction SilentlyContinue
Write-Host "Both instances closed."
exit 0
