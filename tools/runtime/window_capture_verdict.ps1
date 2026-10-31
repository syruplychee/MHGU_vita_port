# Shared capture-evidence policy and helpers for the B49 GUI runner.
#
# Get-WindowCaptureVerdict decides whether a captured frame is USABLE EVIDENCE
# about the owned game window: the window is visible, unoccluded at the sampled
# client points, geometrically and client-origin stable, owned by the retained
# process, and a guest present occurred in this launch. It does NOT decide that
# the pixels are semantically correct game content; that still requires human
# inspection of the preserved PNG. Uniform white/black frames are rejected as
# placeholders. A fully bright but non-uniform frame (textured Chinese UI or
# video) is valid evidence: there is deliberately no black-pixel ratio limit.
#
# The five WindowFromPoint samples are a bounded probe, not proof that every
# client pixel is unoccluded; the receipt records hits/total and the report must
# state that scope.

function Get-WindowCaptureContentClass {
    param([long]$NonBlack = 0, [long]$Samples = 0)
    if ($Samples -le 0) { return 'empty' }
    if ($NonBlack -le 0) { return 'blank' }
    $ratio = [double]$NonBlack / [double]$Samples
    if ($ratio -lt 0.01) { return 'sparse' }
    if ($ratio -lt 0.05) { return 'low-coverage' }
    return 'content'
}

function Get-WindowCaptureVerdict {
    param(
        # Probe/geometry facts are fail-closed: a missing or failed query must
        # never be interpreted as "visible and not cloaked".
        [bool]$ProbeValid = $false,
        [bool]$DwmOk = $false,
        [bool]$GeometryOk = $false,
        [bool]$ClientOk = $false,
        [bool]$OwnerMatch = $false,
        [bool]$Visible = $false,
        [bool]$Iconic = $false,
        [bool]$Cloaked = $false,
        [bool]$GeometryStable = $false,
        [bool]$ClientOriginStable = $false,
        [int]$HitPoints = 0,
        [int]$TotalPoints = 0,
        # Scope is explicitly "at least one present since this launch", not a
        # recency window; the pixel verdict is recomputed for every attempt.
        [bool]$GuestPresentSinceLaunch = $false,
        [long]$Samples = 0,
        [long]$NonBlack = 0,
        [int]$Max = 0,
        [bool]$Uniform = $true,
        [bool]$AllWhite = $false,
        [bool]$AllBlack = $true
    )
    $content = Get-WindowCaptureContentClass -NonBlack $NonBlack -Samples $Samples
    if (-not $ProbeValid) {
        return [pscustomobject]@{ Usable = $false; Reason = 'probe-failed'; ContentClass = $content }
    }
    if (-not $DwmOk) {
        return [pscustomobject]@{ Usable = $false; Reason = 'dwm-query-failed'; ContentClass = $content }
    }
    if (-not $GeometryOk) {
        return [pscustomobject]@{ Usable = $false; Reason = 'geometry-query-failed'; ContentClass = $content }
    }
    if (-not $ClientOk) {
        return [pscustomobject]@{ Usable = $false; Reason = 'client-query-failed'; ContentClass = $content }
    }
    if (-not $OwnerMatch) {
        return [pscustomobject]@{ Usable = $false; Reason = 'owner-mismatch'; ContentClass = $content }
    }
    if (-not $Visible) {
        return [pscustomobject]@{ Usable = $false; Reason = 'not-visible'; ContentClass = $content }
    }
    if ($Iconic) {
        return [pscustomobject]@{ Usable = $false; Reason = 'minimized'; ContentClass = $content }
    }
    if ($Cloaked) {
        return [pscustomobject]@{ Usable = $false; Reason = 'dwm-cloaked'; ContentClass = $content }
    }
    if (-not $GeometryStable -or -not $ClientOriginStable) {
        return [pscustomobject]@{ Usable = $false; Reason = 'geometry-unstable'; ContentClass = $content }
    }
    if ($TotalPoints -le 0 -or $HitPoints -lt $TotalPoints) {
        return [pscustomobject]@{ Usable = $false; Reason = 'occluded'; ContentClass = $content }
    }
    if (-not $GuestPresentSinceLaunch) {
        return [pscustomobject]@{ Usable = $false; Reason = 'guest-present-not-observed'; ContentClass = $content }
    }
    if ($Samples -le 0 -or $Max -le 0) {
        return [pscustomobject]@{ Usable = $false; Reason = 'empty-frame'; ContentClass = $content }
    }
    if ($AllWhite -or $AllBlack -or $Uniform) {
        return [pscustomobject]@{ Usable = $false; Reason = 'uniform-frame'; ContentClass = $content }
    }
    return [pscustomobject]@{ Usable = $true; Reason = 'usable'; ContentClass = $content }
}

# Parse the key=value receipt produced by VPGuiCap::CaptureVerified. The
# post-capture probe repeats some keys after the statistics, so the first
# occurrence is the value reported for that capture. Values are returned as
# strings; the caller decides how to interpret missing keys (treated as false).
function ConvertFrom-VPCaptureReceipt {
    param([string]$Text = '')
    $fields = @{}
    foreach ($part in @(([string]$Text) -split ';')) {
        $at = $part.IndexOf('=')
        if ($at -le 0) { continue }
        $key = $part.Substring(0, $at)
        if (-not $fields.ContainsKey($key)) {
            $fields[$key] = $part.Substring($at + 1)
        }
    }
    return $fields
}

# Strictly parse and normalize a capture schedule. `powershell -File` does not
# split comma-separated arguments, so "-CaptureSeconds 60,85" arrives as one
# string and must be split here. Invalid, negative or overflowing entries throw;
# explicitly supplied values at or after MaxSeconds are recorded as Dropped, not
# silently replaced by the defaults. Valid values are sorted ascending and
# deduplicated. Defaults apply only when no input was supplied at all.
function ConvertTo-VPCaptureSchedule {
    param(
        [object[]]$Values = @(),
        [int]$MaxSeconds = 0,
        [int[]]$Default = @()
    )
    $parsed = New-Object System.Collections.Generic.List[int]
    $dropped = New-Object System.Collections.Generic.List[int]
    $sawInput = $false
    foreach ($raw in @($Values)) {
        if ($null -eq $raw) { continue }
        $rawText = [string]$raw
        if ([string]::IsNullOrWhiteSpace($rawText)) { continue }
        foreach ($part in ($rawText -split ',')) {
            $text = $part.Trim()
            if ($text -eq '') {
                throw 'capture schedule contains an empty entry'
            }
            if ($text -notmatch '^-?\d+$') {
                throw "invalid capture schedule entry '$text' (expected seconds)"
            }
            $value = 0
            if (-not [int]::TryParse($text, [ref]$value)) {
                throw "capture schedule entry out of range: $text"
            }
            if ($value -lt 0) {
                throw "capture schedule entry must be non-negative: $text"
            }
            $sawInput = $true
            if ($MaxSeconds -gt 0 -and $value -ge $MaxSeconds) {
                $dropped.Add($value)
            } else {
                $parsed.Add($value)
            }
        }
    }
    if (-not $sawInput) {
        foreach ($value in @($Default)) {
            $parsedValue = [int]$value
            if ($parsedValue -lt 0) {
                throw "default capture schedule entry must be non-negative: $parsedValue"
            }
            if ($MaxSeconds -gt 0 -and $parsedValue -ge $MaxSeconds) {
                $dropped.Add($parsedValue)
            } else {
                $parsed.Add($parsedValue)
            }
        }
    }
    $sorted = @($parsed | Sort-Object -Unique)
    $normalized = $false
    if ($sorted.Count -ne $parsed.Count) {
        $normalized = $true
    } else {
        for ($i = 0; $i -lt $sorted.Count; $i++) {
            if ([int]$sorted[$i] -ne $parsed[$i]) { $normalized = $true; break }
        }
    }
    return [pscustomobject]@{
        Values = @($sorted | ForEach-Object { [int]$_ })
        Dropped = @($dropped | Sort-Object -Unique | ForEach-Object { [int]$_ })
        Normalized = $normalized
        InputCount = $parsed.Count + $dropped.Count
    }
}

# Decide whether the guest produced a present in THIS launch. The native log is
# rotated/removed before launch; if it was nevertheless recreated since
# SinceUtc, scan from byte 0 even when a previous size was recorded, otherwise
# the initial present markers would be skipped after the file grows past the
# old baseline. A file created before the launch is only accepted from its
# baseline, and if it is shorter than the baseline (truncated) it is rescanned
# from 0. Data dumps additionally require both creation and write times after
# the launch so a cached prior raw file cannot qualify.
#
# The returned scope is "since-launch", not "recent": the caller must still
# capture and validate the current pixels on every attempt.
function Get-VPGuestPresentEvidence {
    param(
        [string]$LogPath = '',
        [long]$BaselineLength = 0,
        [datetime]$SinceUtc = [datetime]::MinValue,
        [string]$DataDir = ''
    )
    $best = 0
    $source = 'none'
    $detail = ''
    $createdSince = $false
    $scannedFrom = 0L
    $logCreation = $null
    if ($LogPath -and (Test-Path -LiteralPath $LogPath)) {
        try {
            $info = Get-Item -LiteralPath $LogPath
            $logCreation = $info.CreationTimeUtc
            $createdSince = $info.CreationTimeUtc -ge $SinceUtc
        } catch {
            $detail = 'log-identity-failed:' + $_.Exception.GetType().Name
        }
        $stream = $null
        try {
            $stream = [IO.File]::Open($LogPath, [IO.FileMode]::Open,
                [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
            $length = $stream.Length
            $start = if ($createdSince -or $length -lt $BaselineLength) { 0L } else { $BaselineLength }
            $scannedFrom = $start
            $window = [Math]::Min([long]1048576, $length - $start)
            if ($window -gt 0) {
                $stream.Seek($length - $window, [IO.SeekOrigin]::Begin) | Out-Null
                $bytes = New-Object byte[] ([int]$window)
                $read = $stream.Read($bytes, 0, $bytes.Length)
                $text = [Text.Encoding]::UTF8.GetString($bytes, 0, $read)
                foreach ($m in [regex]::Matches($text, 'VP_GXM_PERF_PRESENT frames=(\d+)')) {
                    $value = [int]$m.Groups[1].Value
                    if ($value -gt $best) { $best = $value; $source = 'gxm-perf-present' }
                }
                foreach ($m in [regex]::Matches($text, 'VP_GPU_PRESENT count=(\d+)')) {
                    $value = [int]$m.Groups[1].Value
                    if ($value -gt $best) { $best = $value; $source = 'gpu-present' }
                }
                if ($best -le 0 -and $text -match 'VP_DISPLAY_PRESENT') {
                    $best = 1; $source = 'display-present'
                }
            }
        } catch {
            if (-not $detail) { $detail = 'log-read-failed:' + $_.Exception.GetType().Name }
        } finally {
            if ($stream) { $stream.Dispose() }
        }
    }
    if ($best -le 0 -and $DataDir) {
        foreach ($pattern in @('b34-present-*.rgba', 'display-present-*.rgba')) {
            $fresh = @(Get-ChildItem -LiteralPath $DataDir -File -Filter $pattern `
                -ErrorAction SilentlyContinue | Where-Object {
                    $_.Length -gt 0 -and
                    $_.CreationTimeUtc -ge $SinceUtc -and
                    $_.LastWriteTimeUtc -ge $SinceUtc
                })
            if ($fresh.Count -gt 0) {
                $best = $fresh.Count; $source = $pattern
                break
            }
        }
    }
    return [pscustomobject]@{
        PresentSinceLaunch = ($best -gt 0)
        Scope = 'since-launch'
        Source = $source
        Count = $best
        Detail = $detail
        LogCreatedSinceLaunch = $createdSince
        ScannedFrom = $scannedFrom
        LogCreationUtc = if ($logCreation) { $logCreation.ToString('o') } else { '' }
    }
}
