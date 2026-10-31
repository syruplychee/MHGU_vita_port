# B48 bounded graceful window close.
#
# The caller owns the target process. This helper only posts WM_CLOSE to a
# window that is verified to belong to that process and then waits for the
# process to exit. It never terminates anything: a timeout is reported so the
# caller can decide whether to force-stop its own child and record that the
# graceful path failed. Posting to an unrelated window is rejected by PID.
if (-not ('VPWindowCloseNative' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class VPWindowCloseNative {
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);
}
"@
}
if (-not ('VPMiniDumpNative' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class VPMiniDumpNative {
    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool MiniDumpWriteDump(IntPtr hProcess, uint processId,
        IntPtr hFile, uint dumpType, IntPtr exceptionParam, IntPtr userStreamParam,
        IntPtr callbackParam);
}
"@
}

function Close-OwnedWindowGracefully {
    param(
        [Parameter(Mandatory=$true)][IntPtr]$Window,
        [Parameter(Mandatory=$true)][System.Diagnostics.Process]$Process,
        [int]$TimeoutSeconds = 20
    )
    $result = [ordered]@{
        requested = $false
        window_valid = $false
        window_pid = $null
        pid_match = $false
        postmessage_ok = $false
        exit_observed = $false
        timed_out = $false
        timeout_seconds = $TimeoutSeconds
        error = $null
    }
    if ($TimeoutSeconds -le 0) {
        $result.error = 'timeout must be positive'
        return $result
    }
    try {
        # Pin the original process before consulting a reusable window/PID.
        $null = $Process.Handle
        if ($Process.HasExited) {
            $result.error = 'owned process already exited'
            return $result
        }
    } catch {
        $result.error = 'owned process handle unavailable: ' + $_.Exception.Message
        return $result
    }
    if ($Window -eq [IntPtr]::Zero -or -not [VPWindowCloseNative]::IsWindow($Window)) {
        $result.error = 'window-not-valid'
        return $result
    }
    $result.window_valid = $true
    [uint32]$windowPid = 0
    [void][VPWindowCloseNative]::GetWindowThreadProcessId($Window, [ref]$windowPid)
    $result.window_pid = [int]$windowPid
    if ([int]$windowPid -ne [int]$Process.Id) {
        $result.error = 'window-pid-mismatch'
        return $result
    }
    $result.pid_match = $true
    $result.requested = [bool][VPWindowCloseNative]::PostMessage(
        $Window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $result.requested) {
        $result.error = ('postmessage-failed win32=' + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
        return $result
    }
    $result.postmessage_ok = $true
    try {
        $result.exit_observed = [bool]$Process.WaitForExit($TimeoutSeconds * 1000)
    } catch {
        # A retained handle that has already exited still reports via HasExited.
        $result.exit_observed = [bool]$Process.HasExited
    }
    if (-not $result.exit_observed) {
        $result.timed_out = $true
        $result.error = 'graceful close timed out'
    }
    return $result
}

function Write-OwnedProcessMinidump {
    # Diagnostic snapshot of an owned, still-running process (for example a
    # shutdown that did not complete inside its bounded grace). Writes a normal
    # minidump with thread info; the caller records path/size/hash and never
    # treats the dump as a substitute for a real exit result.
    param(
        [Parameter(Mandatory=$true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory=$true)][string]$OutputPath
    )
    $result = [ordered]@{
        written = $false
        path = $OutputPath
        bytes = 0
        sha256 = $null
        error = $null
    }
    $processHandle = [IntPtr]::Zero
    $stream = $null
    try {
        $Process.Refresh()
        if ($Process.HasExited) {
            $result.error = 'owned process already exited'
            return $result
        }
        $expectedStart = $Process.StartTime.ToUniversalTime()
        $expectedPath = $Process.MainModule.FileName
        # Use the handle retained by this Process object. Never OpenProcess by
        # PID here: if the owned process exited and its PID was reused, a fresh
        # PID lookup could dump an unrelated process. This handle is owned by
        # the Process object and must not be closed by us.
        $processHandle = $Process.Handle
        if ($processHandle -eq [IntPtr]::Zero) {
            $result.error = 'owned process handle unavailable'
            return $result
        }
        $Process.Refresh()
        if ($Process.HasExited -or
            $Process.StartTime.ToUniversalTime() -ne $expectedStart -or
            $Process.MainModule.FileName -ne $expectedPath) {
            $result.error = 'owned process identity changed before dump'
            return $result
        }
        $stream = [IO.File]::Create($OutputPath)
        $fileHandle = $stream.SafeFileHandle.DangerousGetHandle()
        $ok = [VPMiniDumpNative]::MiniDumpWriteDump(
            $processHandle, [uint32]$Process.Id, $fileHandle, 0x1000,
            [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
        if (-not $ok) {
            $result.error = ('MiniDumpWriteDump failed win32=' + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
            return $result
        }
        $Process.Refresh()
        $result.process_running_after_dump = -not $Process.HasExited
        $result.written = $true
    } catch {
        $result.error = $_.Exception.Message
    } finally {
        if ($stream) { $stream.Dispose() }
    }
    if ($result.written -and (Test-Path -LiteralPath $OutputPath)) {
        $result.bytes = (Get-Item -LiteralPath $OutputPath).Length
        $result.sha256 = (Get-FileHash -LiteralPath $OutputPath -Algorithm SHA256).Hash
    }
    return $result
}
