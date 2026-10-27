#Requires -Version 5.1
<#
.SYNOPSIS
    Sample interactive-desktop state once per second and log it, to measure what
    an RDP disconnect actually does. GitHub issue #132.

.DESCRIPTION
    runner_watchdog.ps1 pauses the runner when OpenInputDesktop fails, on the
    premise that an RDP disconnect leaves the session alive with no input
    desktop. That premise is REASONED -- from the OpenInputDesktop contract, and
    from smoke_uia.py depending on the same call -- and has never been measured.
    Only the positive case was ever observed: session Active, probe SUCCESS.

    This script measures the negative case. It must be launched DETACHED, because
    the whole point is that it keeps running while the operator is gone and there
    is no desktop to launch anything from.

    Four independent signals per sample, because they disagree in informative
    ways and one alone cannot distinguish "disconnected" from "locked":

      OpenInputDesktop   the signal the watchdog actually uses. Should fail when
                         no desktop is receiving input.
      OpenDesktop("Default")  opens the desktop BY NAME rather than by input
                         focus. Expected to keep succeeding across a disconnect
                         -- if it does, that is positive proof the session is
                         alive and only the INPUT desktop went away, which is
                         exactly the distinction the watchdog relies on.
      WTS connect state  Active / Disconnected / Idle for this session.
      console session    which session owns the physical console.

.NOTES
    Read-only. Opens desktop handles and closes them; queries WTS. Changes
    nothing, starts nothing, stops nothing. It cannot disturb a runner.
#>
[CmdletBinding()]
param(
    [string]$LogPath = (Join-Path $env:TEMP 'pdfe-desktop-probe.log'),
    [int]$DurationSeconds = 300,
    [int]$IntervalSeconds = 1
)

$ErrorActionPreference = 'Stop'

if (-not ([System.Management.Automation.PSTypeName]'PdfeDesktopProbe132').Type) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class PdfeDesktopProbe132 {
    [DllImport("user32.dll", SetLastError=true)]
    public static extern IntPtr OpenInputDesktop(uint flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    public static extern IntPtr OpenDesktop(string name, uint flags, bool inherit, uint access);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool CloseDesktop(IntPtr h);
    [DllImport("kernel32.dll")]
    public static extern uint WTSGetActiveConsoleSessionId();
    [DllImport("wtsapi32.dll", SetLastError=true)]
    public static extern bool WTSQuerySessionInformationW(
        IntPtr server, uint sessionId, int infoClass, out IntPtr buffer, out uint bytes);
    [DllImport("wtsapi32.dll")]
    public static extern void WTSFreeMemory(IntPtr p);

    // Generic read access is enough to answer "is there one"; asking for more
    // could fail for permission reasons and be misread as "no desktop".
    const uint DESKTOP_READOBJECTS = 0x0001;

    public static string InputDesktop() {
        IntPtr h = OpenInputDesktop(0, false, DESKTOP_READOBJECTS);
        if (h == IntPtr.Zero) return "FAIL(" + Marshal.GetLastWin32Error() + ")";
        CloseDesktop(h);
        return "OK";
    }
    public static string NamedDesktop(string name) {
        IntPtr h = OpenDesktop(name, 0, false, DESKTOP_READOBJECTS);
        if (h == IntPtr.Zero) return "FAIL(" + Marshal.GetLastWin32Error() + ")";
        CloseDesktop(h);
        return "OK";
    }
    public static string ConnectState(uint sessionId) {
        IntPtr buf; uint n;
        // WTSConnectState = 8
        if (!WTSQuerySessionInformationW(IntPtr.Zero, sessionId, 8, out buf, out n))
            return "QUERY-FAIL(" + Marshal.GetLastWin32Error() + ")";
        int v = Marshal.ReadInt32(buf);
        WTSFreeMemory(buf);
        switch (v) {
            case 0: return "Active";
            case 1: return "Connected";
            case 2: return "ConnectQuery";
            case 3: return "Shadow";
            case 4: return "Disconnected";
            case 5: return "Idle";
            case 6: return "Listen";
            case 7: return "Reset";
            case 8: return "Down";
            case 9: return "Init";
            default: return "Unknown(" + v + ")";
        }
    }
    public static uint ConsoleSession() { return WTSGetActiveConsoleSessionId(); }
}
'@ -Language CSharp
}

$sessionId = (Get-Process -Id $PID).SessionId

function Write-Sample([string]$text) {
    $line = "{0}  {1}" -f (Get-Date).ToString('yyyy-MM-ddTHH:mm:ss.fff'), $text
    Add-Content -Path $LogPath -Value $line -Encoding UTF8
}

Write-Sample ("START pid={0} session={1} duration={2}s interval={3}s" -f `
              $PID, $sessionId, $DurationSeconds, $IntervalSeconds)
Write-Sample "COLUMNS  inputDesktop | namedDesktop(Default) | wtsState | consoleSession"

$deadline = (Get-Date).AddSeconds($DurationSeconds)
$prev = $null
while ((Get-Date) -lt $deadline) {
    $input_  = [PdfeDesktopProbe132]::InputDesktop()
    $named   = [PdfeDesktopProbe132]::NamedDesktop('Default')
    $wts     = [PdfeDesktopProbe132]::ConnectState($sessionId)
    $console = [PdfeDesktopProbe132]::ConsoleSession()

    $now = "{0,-12} {1,-12} {2,-14} console={3}" -f $input_, $named, $wts, $console
    # Log every sample, but mark transitions so the disconnect is unmissable in
    # a 300-line file. A state change is the entire finding.
    if ($now -ne $prev) {
        Write-Sample ("CHANGE  " + $now)
        $prev = $now
    } else {
        Write-Sample ("        " + $now)
    }
    Start-Sleep -Seconds $IntervalSeconds
}
Write-Sample "END"
