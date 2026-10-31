# Observe one process until it exits, recording the real exit code and exact
# elapsed time from a retained process handle. Never probes Get-Process by PID
# at exit time to decide the result (that can observe a reused PID), and never
# kills a process it did not launch unless -StopAfterSeconds was requested for
# its own child.
#
# Modes:
#   -Launch -FilePath <exe> -ArgumentsJson '["arg","with space"]'
#                                                 start and observe an owned child
#   -AttachPid <pid> [-ExpectedStartTimeUtc <o>]  observe an existing process
#
# The output JSON classifies the observation as exited / still_alive /
# attach_rejected. "caller_stop" only means this tool stopped its own child;
# a nonzero exit without caller_stop is reported as "crash_suspected" but is
# not proof of a guest crash (the host runner, a job object, or the OS may have
# terminated it). Log hashes are marked unstable when the file size changes
# during hashing.
#
# Exit codes: 0 exit observed, 2 still alive at deadline, 3 attach rejected,
# 4 usage error, 5 the observer itself failed.

[CmdletBinding()]
param(
    [switch]$Launch,
    [int]$AttachPid = 0,
    [string]$ExpectedStartTimeUtc = '',
    [string]$FilePath = '',
    # JSON array of strings. A PowerShell array cannot be passed as one
    # -Arguments value reliably (the binder flattens it into separate
    # arguments), so the child argument vector is passed as JSON.
    [string]$ArgumentsJson = '',
    [string]$WorkingDirectory = '',
    [string]$OutputPath = '',
    [string]$ReadyPath = '',
    [string[]]$LogPath = @(),
    [string]$LogPathsJson = '',
    # File-based transport for the log path array. Start-Process joins an
    # argument array into one command line without preserving JSON quoting, so
    # passing LogPathsJson directly loses its brackets/quotes. A file keeps the
    # argument vector unambiguous and is readable evidence for the run.
    [string]$LogPathsFile = '',
    [string]$ProcessName = '',
    [int]$PollMilliseconds = 500,
    [int]$MaxSeconds = 0,
    [int]$StopAfterSeconds = 0,
    [int]$TailBytes = 8192,
    [switch]$NoCrashEvents
)

$ErrorActionPreference = 'Stop'
if ($LogPathsJson -and $LogPathsFile) {
    throw 'Specify either LogPathsJson or LogPathsFile, not both'
}
if ($LogPathsFile) {
    $LogPathsJson = Get-Content -LiteralPath $LogPathsFile -Raw -Encoding UTF8
}
if ($LogPathsJson) {
    $jsonText = ([string]$LogPathsJson).Trim()
    if ($jsonText.Length -lt 2 -or -not $jsonText.StartsWith('[') -or
        -not $jsonText.EndsWith(']')) {
        throw 'LogPathsJson must be a bounded JSON array'
    }
    # PowerShell 7 unrolls a single-element JSON array to a scalar while
    # Windows PowerShell returns the array; iterate the parsed value so both
    # shapes yield the same string sequence after the array-shape check.
    $paths = New-Object System.Collections.Generic.List[string]
    foreach ($path in (ConvertFrom-Json -InputObject $jsonText)) {
        if ($path -isnot [string]) { throw 'Log path must be a string' }
        $paths.Add([string]$path)
    }
    if ($paths.Count -gt 16) { throw 'LogPathsJson must be a bounded JSON array' }
    $LogPath = [string[]]$paths.ToArray()
}
$StartTimeToleranceTicks = 10000L # 1 ms; compare StartTime as precise ticks

if (-not ('VPLifecycleNative' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class VPLifecycleNative {
    [DllImport("kernel32.dll")] public static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool IsProcessInJob(IntPtr process, IntPtr job, out bool result);
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool QueryInformationJobObject(
        IntPtr job, int infoClass, IntPtr info, int length, out int returned);
}
"@
}

function Get-StartTimeUtc([System.Diagnostics.Process]$Process) {
    try { return $Process.StartTime.ToUniversalTime() } catch { return $null }
}

function ConvertTo-Utc([string]$Text) {
    if (-not $Text) { return $null }
    try {
        $parsed = [datetime]::Parse($Text,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::RoundtripKind)
        return $parsed.ToUniversalTime()
    } catch { return $null }
}

function Get-JobInfo([System.Diagnostics.Process]$Target) {
    $info = [ordered]@{
        observe_supported = $true
        observer_in_job = $null
        target_in_observer_job = $null
        limit_flags = $null
        active_process_limit = $null
        kill_on_job_close = $null
        win32_error = $null
    }
    try {
        $selfInJob = $false
        $targetInJob = $false
        if ([VPLifecycleNative]::IsProcessInJob(
                [VPLifecycleNative]::GetCurrentProcess(), [IntPtr]::Zero,
                [ref]$selfInJob)) {
            $info.observer_in_job = $selfInJob
        } else {
            $info.win32_error = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
        }
        if ($Target -and $Target.Handle -ne [IntPtr]::Zero) {
            if ([VPLifecycleNative]::IsProcessInJob(
                    $Target.Handle, [IntPtr]::Zero, [ref]$targetInJob)) {
                $info.target_in_observer_job = $targetInJob
            }
        }
        # QueryInformationJobObject with a NULL job handle uses the job of the
        # calling process. JOBOBJECT_EXTENDED_LIMIT_INFORMATION is 144 bytes on
        # this 64-bit host; LimitFlags is at +16 and ActiveProcessLimit at +40.
        $buffer = [Runtime.InteropServices.Marshal]::AllocHGlobal(144)
        try {
            for ($i = 0; $i -lt 144; $i += 4) {
                [Runtime.InteropServices.Marshal]::WriteInt32($buffer, $i, 0)
            }
            $returned = 0
            if ([VPLifecycleNative]::QueryInformationJobObject(
                    [IntPtr]::Zero, 9, $buffer, 144, [ref]$returned)) {
                $flags = [Runtime.InteropServices.Marshal]::ReadInt32($buffer, 16)
                $info.limit_flags = ('0x{0:X8}' -f [uint32]([int64]$flags -band 0xffffffffL))
                $info.active_process_limit =
                    [Runtime.InteropServices.Marshal]::ReadInt32($buffer, 40)
                $info.kill_on_job_close = (($flags -band 0x2000) -ne 0)
            } else {
                $info.win32_error = [Runtime.InteropServices.Marshal]::GetLastWin32Error()
            }
        } finally {
            [Runtime.InteropServices.Marshal]::FreeHGlobal($buffer)
        }
    } catch {
        $info.observe_supported = $false
        $info.win32_error = $_.Exception.Message
    }
    return $info
}

function ConvertTo-AsciiJson($Value) {
    # The record can embed localized Windows event-log text. Escape every
    # non-ASCII UTF-16 code unit so any consumer decodes the file identically
    # regardless of whether it assumes ANSI or UTF-8 for a no-BOM file.
    $json = $Value | ConvertTo-Json -Depth 8
    $builder = New-Object System.Text.StringBuilder
    foreach ($ch in $json.ToCharArray()) {
        if ([int]$ch -lt 0x80) { [void]$builder.Append($ch) }
        else { [void]$builder.AppendFormat('\u{0:x4}', [int]$ch) }
    }
    return $builder.ToString()
}

function Write-JsonFile([string]$Path, $Value) {
    $full = [IO.Path]::GetFullPath($Path)
    $directory = [IO.Path]::GetDirectoryName($full)
    if ($directory) { New-Item -ItemType Directory -Force $directory | Out-Null }
    $temporary = "$full.tmp-$PID"
    [IO.File]::WriteAllText($temporary, (ConvertTo-AsciiJson $Value),
        (New-Object System.Text.UTF8Encoding($false)))
    Move-Item -LiteralPath $temporary -Destination $full -Force
}

function Write-Result($Result, [int]$Code) {
    $json = ConvertTo-AsciiJson $Result
    if ($OutputPath) { Write-JsonFile $OutputPath $Result }
    Write-Output $json
    exit $Code
}

function Get-LogEvidence([string]$Path) {
    $item = [ordered]@{ path = $Path; exists = $false; stable = $false }
    try {
        if (-not (Test-Path -LiteralPath $Path)) { return $item }
        $before = (Get-Item -LiteralPath $Path -ErrorAction Stop).Length
        $item.exists = $true
        $item.bytes = $before
        $item.sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256 -ErrorAction Stop).Hash
        if ($TailBytes -gt 0 -and $before -gt 0) {
            $length = [int][Math]::Min($before, $TailBytes)
            $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
            try {
                $stream.Seek(-$length, 'End') | Out-Null
                $buffer = New-Object byte[] $length
                $read = $stream.Read($buffer, 0, $length)
                $text = [Text.Encoding]::UTF8.GetString($buffer, 0, $read)
                $item.tail_bytes = $read
                # Keep the JSON bounded and avoid splitting a UTF-8 sequence.
                $item.tail_text = if ($text.Length -gt 8192) { $text.Substring($text.Length - 8192) } else { $text }
            } finally { $stream.Dispose() }
        }
        $after = (Get-Item -LiteralPath $Path -ErrorAction Stop).Length
        $item.size_after = $after
        $item.stable = ($before -eq $after)
        if (-not $item.stable) {
            $item.snapshot_note = 'file size changed while hashing; sha256/tail are not a final snapshot'
        }
    } catch {
        $item.error = $_.Exception.Message
    }
    return $item
}

function Get-CrashEvents([string]$Name, [datetime]$FromUtc, [datetime]$ToUtc) {
    $events = @()
    if (-not $Name) { return $events }
    try {
        $raw = Get-WinEvent -FilterHashtable @{
            LogName = 'Application'; StartTime = $FromUtc; EndTime = $ToUtc
        } -ErrorAction Stop
        foreach ($event in $raw) {
            $provider = [string]$event.ProviderName
            if ($provider -notmatch 'Application Error|Windows Error Reporting|\.NET Runtime') { continue }
            $message = [string]$event.Message
            if ($message -notmatch [regex]::Escape($Name)) { continue }
            $events += [ordered]@{
                time_utc = $event.TimeCreated.ToUniversalTime().ToString('o')
                provider = $provider
                id = $event.Id
                level = $event.LevelDisplayName
                message = if ($message.Length -gt 500) { $message.Substring(0, 500) } else { $message }
            }
            if ($events.Count -ge 8) { break }
        }
    } catch {
        return [ordered]@{ event_log_error = $_.Exception.Message }
    }
    return $events
}

$result = [ordered]@{
    schema = 'vp-process-lifecycle-v1'
    observer_pid = $PID
    mode = $null
    classification = 'unknown'
    started_utc = $null
    ended_utc = $null
    elapsed_seconds = $null
    exit_code = $null
    exit_code_hex = $null
    caller_stop = $false
    crash_suspected = $false
    natural_exit_suspected = $false
    pid_reused = $false
    pid_alive_after_exit = $false
    handle_opened = $false
    expected_start_time_checked = $false
    expected_start_time_utc = $ExpectedStartTimeUtc
    pid = $null
    process_name = $ProcessName
    max_seconds = $MaxSeconds
    job = $null
    logs = @()
    crash_events = @()
    error = $null
}

$launched = $false
$proc = $null
try {
    if ($AttachPid -gt 0 -and ($Launch -or $FilePath)) {
        $result.error = 'Specify either -AttachPid or -Launch -FilePath, not both'
        Write-Result $result 4
    }
    if ($AttachPid -gt 0) {
        $result.mode = 'attach'
        $result.pid = $AttachPid
        try { $proc = [System.Diagnostics.Process]::GetProcessById($AttachPid) }
        catch {
            $result.classification = 'attach_rejected'
            $result.error = "no process with PID $AttachPid"
            Write-Result $result 3
        }
        $start = Get-StartTimeUtc $proc
        if ($null -eq $start) {
            $result.classification = 'attach_rejected'
            $result.error = 'process start time is not readable; refusing to observe'
            Write-Result $result 3
        }
        if ($ExpectedStartTimeUtc) {
            $expected = ConvertTo-Utc $ExpectedStartTimeUtc
            if ($null -eq $expected) {
                $result.classification = 'attach_rejected'
                $result.error = 'ExpectedStartTimeUtc is not a valid timestamp'
                Write-Result $result 3
            }
            $result.expected_start_time_checked = $true
            $deltaTicks = [Math]::Abs(($start - $expected).Ticks)
            if ($deltaTicks -gt $StartTimeToleranceTicks) {
                $result.classification = 'attach_rejected'
                $result.error = "start time mismatch ($deltaTicks ticks); possible PID reuse"
                $result.observed_start_utc = $start.ToString('o')
                Write-Result $result 3
            }
        }
        $result.started_utc = $start.ToString('o')
        if (-not $result.process_name) { $result.process_name = $proc.ProcessName }
    } elseif ($Launch -or $FilePath) {
        if (-not $FilePath) {
            $result.error = '-Launch requires -FilePath'
            Write-Result $result 4
        }
        $result.mode = 'launch'
        $childArguments = @()
        if ($ArgumentsJson) {
            try {
                $parsed = $ArgumentsJson | ConvertFrom-Json
                $childArguments = @($parsed | ForEach-Object { [string]$_ })
            } catch {
                $result.error = "ArgumentsJson is not a JSON string array: $($_.Exception.Message)"
                Write-Result $result 4
            }
        }
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $FilePath
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
        if ($WorkingDirectory) { $startInfo.WorkingDirectory = $WorkingDirectory }
        if ($startInfo.PSObject.Properties.Name -contains 'ArgumentList') {
            foreach ($argument in $childArguments) { $startInfo.ArgumentList.Add([string]$argument) }
        } else {
            $quoted = foreach ($argument in $childArguments) {
                $text = [string]$argument
                if ($text -match '[\s"]') { '"' + ($text -replace '"', '\"') + '"' } else { $text }
            }
            $startInfo.Arguments = ($quoted -join ' ')
        }
        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $startInfo
        if (-not $proc.Start()) {
            $result.error = 'process failed to start'
            Write-Result $result 5
        }
        $launched = $true
        $result.pid = $proc.Id
        $start = Get-StartTimeUtc $proc
        if ($null -eq $start) { $start = [datetime]::UtcNow }
        $result.started_utc = $start.ToString('o')
        if (-not $result.process_name) {
            try { $result.process_name = $proc.ProcessName } catch { }
        }
    } else {
        $result.error = 'Specify -AttachPid or -Launch -FilePath'
        Write-Result $result 4
    }

    # Open and retain the process handle now, before the child can exit. All
    # later exit/exit-code reads use this handle, not a fresh PID lookup.
    try {
        $null = $proc.Handle
        $result.handle_opened = $true
    } catch {
        $result.handle_opened = $false
        $result.error = "retained handle unavailable: $($_.Exception.Message)"
    }

    $startUtc = ConvertTo-Utc $result.started_utc
    if ($null -eq $startUtc) { $startUtc = [datetime]::UtcNow }
    $result.job = Get-JobInfo $proc
    if ($ReadyPath) {
        $ready = [ordered]@{
            schema = 'vp-process-lifecycle-ready-v1'
            observer_pid = $PID
            target_pid = $proc.Id
            started_utc = $result.started_utc
            handle_opened = $result.handle_opened
            job = $result.job
            written_utc = [datetime]::UtcNow.ToString('o')
        }
        try { Write-JsonFile $ReadyPath $ready }
        catch { $result.error = "ready receipt failed: $($_.Exception.Message)" }
    }

    $deadline = $null
    if ($MaxSeconds -gt 0) { $deadline = [datetime]::UtcNow.AddSeconds($MaxSeconds) }
    $stoppedByUs = $false

    while ($true) {
        if ($proc.WaitForExit($PollMilliseconds)) { break }
        $now = [datetime]::UtcNow
        if ($StopAfterSeconds -gt 0 -and $launched -and
            ($now - $startUtc).TotalSeconds -ge $StopAfterSeconds) {
            # Only our own launched child may be stopped. The process object
            # keeps the original handle, so this cannot hit a reused PID.
            if ($proc.HasExited) { break }
            $result.caller_stop = $true
            $stoppedByUs = $true
            try { $proc.Kill() } catch { $result.error = "stop failed: $($_.Exception.Message)" }
            if (-not $proc.WaitForExit(5000)) {
                $result.error = 'owned child did not exit after stop request'
            }
            break
        }
        if ($null -ne $deadline -and $now -ge $deadline) { break }
    }

    $exited = $false
    try { $exited = $proc.HasExited } catch { }
    if (-not $exited) {
        $result.classification = 'still_alive'
        $result.ended_utc = [datetime]::UtcNow.ToString('o')
        $result.elapsed_seconds = [Math]::Round(([datetime]::UtcNow - $startUtc).TotalSeconds, 3)
        $result.logs = @($LogPath | ForEach-Object { Get-LogEvidence $_ })
        Write-Result $result 2
    }

    try {
        $exitCode = $proc.ExitCode
        $result.exit_code = $exitCode
        $unsignedExit = [uint32]([int64]$exitCode -band 0xffffffffL)
        $result.exit_code_hex = ('0x{0:X8}' -f $unsignedExit)
        $result.crash_suspected = (-not $stoppedByUs) -and ($unsignedExit -ge 0x80000000L)
        $result.natural_exit_suspected = (-not $stoppedByUs) -and ($exitCode -eq 0)
    } catch {
        $result.error = "exit code unavailable: $($_.Exception.Message)"
    }
    try { $result.ended_utc = $proc.ExitTime.ToUniversalTime().ToString('o') } catch { }
    if ($result.ended_utc) {
        $end = ConvertTo-Utc $result.ended_utc
        if ($null -ne $end) {
            $result.elapsed_seconds = [Math]::Round(($end - $startUtc).TotalSeconds, 3)
        }
    }
    $result.classification = 'exited'

    # PID reuse is decided against the retained handle's recorded start time,
    # not by trusting a fresh Get-Process lookup for the same PID.
    try {
        $current = [System.Diagnostics.Process]::GetProcessById($result.pid)
        $currentStart = Get-StartTimeUtc $current
        if ($null -ne $currentStart) {
            if ([Math]::Abs(($currentStart - $startUtc).Ticks) -gt $StartTimeToleranceTicks) {
                $result.pid_reused = $true
            } else {
                $result.pid_alive_after_exit = $true
            }
        }
    } catch { }
    $result.logs = @($LogPath | ForEach-Object { Get-LogEvidence $_ })
    if (-not $NoCrashEvents) {
        try {
            $events = Get-CrashEvents $result.process_name `
                $startUtc.AddSeconds(-5) ([datetime]::UtcNow.AddSeconds(10))
            $result.crash_events = @($events)
        } catch { $result.crash_events = @() }
    }
    Write-Result $result 0
} catch {
    $result.error = $_.Exception.Message
    $result.logs = @($LogPath | ForEach-Object { Get-LogEvidence $_ })
    Write-Result $result 5
}
