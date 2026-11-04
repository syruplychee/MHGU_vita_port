# Launch Vita3K in GUI mode with a fixed configuration, capture the visible
# game window at requested times, and (by default) stop the emulator.
#
# Console mode never initializes SDL, so a visible present can only be checked
# in GUI mode. The default visible-window capture is a bounded CopyFromScreen.
# PrintWindow/BitBlt are opt-in diagnostics: they can block the target's UI
# thread and must not replace the actual window capture in acceptance evidence.
param(
    [string]$Config = "",
    [string]$OutDir = "",
    # Strings, not int[]: `powershell -File` does not split "60,85" into two
    # arguments, and an int[] conversion would silently read it as 6085.
    [string[]]$CaptureSeconds = @(20, 35),
    # Applied only when CaptureSeconds contains no input at all. The batch
    # wrapper supplies its historical 30/120/180/240 default explicitly.
    [int[]]$CaptureDefaultSeconds = @(20, 35),
    # GPU/oracle runs can explicitly omit visible-window evidence.
    [switch]$NoWindowCapture,
    [int]$ExitAfterSeconds = 0,
    [switch]$KeepRunning,
    [switch]$DiagnosticMode,
    [switch]$PrepareOnly,
    [switch]$BoundedBatch,
    [switch]$LiveInput,
    [switch]$PlayPerf,
    [switch]$LiveCapture,
    [switch]$SilentAudio,
    [switch]$CaptureShaders,
    [switch]$CaptureModels,
    [switch]$SurfaceReadback,
    [switch]$CaptureAudio,
    [string]$Replay = "",
    [string]$Emulator = "",
    [string]$Vpk = "",
    [string]$StaticResourcePackageDir = "",
    [string]$StaticResourceSidecar = "",
    [string]$StaticResourceManifest = "",
    [string]$StaticShaderSource = "",
    [switch]$NoDetach,
    [switch]$ProcessCheckSelfTest,
    # B48 lifecycle: post WM_CLOSE to the owned window and wait for the real
    # process exit. A timeout is recorded and, with -RequireGracefulExit, fails
    # the run before any force stop is used as cleanup.
    [int]$GracefulExitSeconds = 0,
    [switch]$RequireGracefulExit,
    [string]$RequireAudioAdapter = "",
    # Diagnostic: write a minidump of the owned process if the bounded WM_CLOSE
    # grace expires, so a shutdown hang is analysable without a debugger.
    [switch]$CaptureShutdownDump,
    # Diagnostic only: additionally try PrintWindow and a screen BitBlt. Both
    # are off by default because they can block a hung owner and are not
    # acceptance evidence for the actual window contents.
    [switch]$CaptureCompatibilityShots,
    # B49: a capture is accepted only after the guest has presented at least
    # once in this launch and the owned window is visible, unoccluded and
    # geometrically stable. This bounds how long a capture may wait for those
    # conditions; zero keeps the historical single-shot behaviour.
    [int]$CaptureReadyTimeoutSeconds = 60
)
$ErrorActionPreference = "Stop"

# Resolve DPI awareness before any window geometry is sampled. If this process
# stays DPI-unaware, GetWindowRect/GetClientRect and CopyFromScreen disagree
# about physical versus logical pixels; a later switch can also make the crop
# drift between enumeration and capture. Qt normally sets per-monitor-v2 first,
# in which case the call is rejected and the existing awareness is kept.
if (-not ('VPDpiContext' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class VPDpiContext {
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr value);
    [DllImport("user32.dll")] static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] static extern IntPtr GetThreadDpiAwarenessContext();
    [DllImport("user32.dll")] static extern int GetAwarenessFromDpiAwarenessContext(IntPtr context);
    static string Current() {
        try {
            int value = GetAwarenessFromDpiAwarenessContext(GetThreadDpiAwarenessContext());
            if (value == 2) return "per-monitor";
            if (value == 1) return "system";
            if (value == 0) return "unaware";
            return "value-" + value;
        } catch { return "unknown"; }
    }
    public static string Initialize() {
        string before = Current();
        try { if (SetProcessDpiAwarenessContext(new IntPtr(-4))) return before + "->per-monitor-v2"; }
        catch { }
        try { if (SetProcessDPIAware()) return before + "->system"; }
        catch { }
        return before + "->unchanged";
    }
}
"@
}
$script:DpiAwareness = [VPDpiContext]::Initialize()

# Capture schedule and the pure evidence policy are validated before any
# savedata/marker/process setup, so an invalid or out-of-budget schedule cannot
# start a run. The batch wrapper performs the same strict parsing before it
# stages savedata.
. (Join-Path $PSScriptRoot 'window_capture_verdict.ps1')
$captureScheduleInfo = if ($NoWindowCapture) {
    [pscustomobject]@{ Values = @(); Dropped = @(); Normalized = $false; InputCount = 0 }
} else {
    ConvertTo-VPCaptureSchedule -Values $CaptureSeconds `
        -MaxSeconds $ExitAfterSeconds -Default $CaptureDefaultSeconds
}
$captureSchedule = @($captureScheduleInfo.Values)

# A terminated-but-unreaped Windows process can stay visible to Get-Process with
# HasExited=true, no window, and no executable path. Treating it as a live
# emulator blocks every later launch. Liveness is decided by HasExited (plus a
# live window handle as a conservative second signal), never by window presence
# alone: a headless active instance must still block a new launch.
function Get-ActiveVita3KProcess {
    param([string]$Name = "Vita3K")
    $active = New-Object System.Collections.Generic.List[object]
    $processes = @(Get-Process -Name $Name -ErrorAction SilentlyContinue)
    if ($processes.Count -eq 0) { return @() }
    $exeName = if ($Name -match '\.exe$') { $Name } else { "$Name.exe" }
    $cimByPid = @{}
    try {
        Get-CimInstance -ClassName Win32_Process -Filter "Name='$exeName'" -ErrorAction Stop |
            ForEach-Object { $cimByPid[[int]$_.ProcessId] = $_ }
    } catch { }
    foreach ($p in $processes) {
        $exited = $false
        try { $exited = [bool]$p.HasExited } catch { $exited = $false }
        $hwnd = [int64]0
        try { $hwnd = [int64]$p.MainWindowHandle } catch { }
        $info = $cimByPid[[int]$p.Id]
        $exe = ""
        if ($info -and $info.ExecutablePath) { $exe = [string]$info.ExecutablePath }
        $ws = [int64]0
        try { $ws = [int64]$p.WorkingSet64 } catch { }
        if ($exited -and $hwnd -eq 0) {
            $script:Vita3KSkippedEntries += ,([pscustomobject]@{
                Id = $p.Id; HasExited = $exited; ExecutablePath = $exe; WorkingSet64 = $ws
            })
            continue
        }
        $active.Add([pscustomobject]@{
            Id = $p.Id; HasExited = $exited; ExecutablePath = $exe
            MainWindowHandle = $hwnd; WorkingSet64 = $ws
            CommandLine = $(if ($info) { [string]$info.CommandLine } else { "" })
            CreationDate = $(if ($info) { $info.CreationDate } else { $null })
        })
    }
    return $active.ToArray()
}

# FileInfo has no Hash property. Use Get-FileHash explicitly so static shader
# identity is complete; a missing or unreadable GXP must abort rather than
# record a null hash. The same function is exercised by the offline hash check.
function Get-FileSha256Hex {
    param([Parameter(Mandatory=$true)][string]$LiteralPath)
    $fileHash = Get-FileHash -LiteralPath $LiteralPath -Algorithm SHA256 -ErrorAction Stop
    if ($null -eq $fileHash -or [string]::IsNullOrWhiteSpace($fileHash.Hash)) {
        throw "SHA256 calculation returned no hash: $LiteralPath"
    }
    return [string]$fileHash.Hash
}

# B48: choose the visible window that most likely holds game pixels. A 960x544
# client area is the GXM display size; the launcher main window is larger.
# Falling back to the process main window keeps the old bounded behaviour.
function Select-VPGameCaptureWindow {
    param([string[]]$Windows = @(), [IntPtr]$Fallback = [IntPtr]::Zero)
    $best = $Fallback
    $bestScore = -1
    $bestInfo = ('fallback=' + $Fallback.ToInt64())
    foreach ($line in @($Windows)) {
        $m = [regex]::Match($line,
            '^(?<h>\d+)\|vis=(?<vis>True|False)\|rect=(?<rl>-?\d+),(?<rt>-?\d+),(?<rr>-?\d+),(?<rb>-?\d+)\|client=(?<w>\d+)x(?<hgt>\d+)\|title=(?<title>.*)$')
        if (-not $m.Success -or $m.Groups['vis'].Value -ne 'True') { continue }
        $w = [int]$m.Groups['w'].Value
        $h = [int]$m.Groups['hgt'].Value
        if ($w -le 0 -or $h -le 0) { continue }
        $score = 10 + [Math]::Min([int]($w * $h / 100000), 20)
        if ($w -eq 960 -and $h -eq 544) { $score += 1000 }
        if ($m.Groups['title'].Value -match 'MHGU|Bilinear|Vulkan') { $score += 50 }
        if ($score -gt $bestScore) {
            $bestScore = $score
            $best = [IntPtr][int64]$m.Groups['h'].Value
            $bestInfo = $line
        }
    }
    return [pscustomobject]@{ Handle = $best; Info = $bestInfo; Score = $bestScore }
}

# B48: WM_CLOSE on the game window only returns to the launcher; the emulator
# process exits through the main Vita3K window's closeEvent. Select that window
# explicitly instead of Process.MainWindowHandle, which reports the game window
# while a title is running.
function Select-VPMainWindow {
    param([string[]]$Windows = @(), [IntPtr]$Fallback = [IntPtr]::Zero)
    $best = $Fallback
    $bestScore = -1
    $bestInfo = ('fallback=' + $Fallback.ToInt64())
    foreach ($line in @($Windows)) {
        $m = [regex]::Match($line,
            '^(?<h>\d+)\|vis=(?<vis>True|False)\|rect=(?<rl>-?\d+),(?<rt>-?\d+),(?<rr>-?\d+),(?<rb>-?\d+)\|client=(?<w>\d+)x(?<hgt>\d+)\|title=(?<title>.*)$')
        if (-not $m.Success -or $m.Groups['vis'].Value -ne 'True') { continue }
        $w = [int]$m.Groups['w'].Value
        $h = [int]$m.Groups['hgt'].Value
        # A minimized Qt launcher reports a 0x0 client but still owns the
        # process closeEvent. Geometry is a capture requirement, not a close
        # requirement; Close-OwnedWindowGracefully separately verifies the PID.
        $isLauncher = $m.Groups['title'].Value -match '^Vita3K\b'
        if (($w -le 0 -or $h -le 0) -and -not $isLauncher) { continue }
        $score = [int]($w * $h / 10000)
        if ($isLauncher) { $score += 100000 }
        if ($m.Groups['title'].Value -match 'MHGU|Bilinear|Vulkan') { $score -= 100000 }
        if ($score -gt $bestScore) {
            $bestScore = $score
            $best = [IntPtr][int64]$m.Groups['h'].Value
            $bestInfo = $line
        }
    }
    return [pscustomobject]@{ Handle = $best; Info = $bestInfo; Score = $bestScore }
}

# B49: guest-present evidence, the capture verdict, the receipt parser and the
# strict schedule parser live in window_capture_verdict.ps1 so they can be
# tested without launching the emulator.

# The sidecar/runtime bind the *whole* shader-index.bin CRC32.  The word at
# offset 44 is only the index body CRC and is a different domain.
if (-not ('VPIndexCrc32' -as [type])) {
    Add-Type -TypeDefinition @'
public static class VPIndexCrc32 {
    public static uint Compute(byte[] data) {
        unchecked {
            uint crc = 0xFFFFFFFFu;
            foreach (byte value in data) {
                crc ^= value;
                for (int bit = 0; bit < 8; ++bit)
                    crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
            }
            return ~crc;
        }
    }
}
'@
}

if ($ProcessCheckSelfTest) {
    $script:Vita3KSkippedEntries = @()
    $hostPath = (Get-Process -Id $PID).Path
    $hostName = (Get-Process -Id $PID).ProcessName
    $failed = $false
    $live = $null; $dead = $null; $deadExit = $false
    try {
        $live = Start-Process -FilePath $hostPath -ArgumentList @(
            "-NoProfile", "-Command", "Start-Sleep -Seconds 15") -PassThru -WindowStyle Hidden
        Start-Sleep -Milliseconds 800
        $seen = @(Get-ActiveVita3KProcess -Name $hostName | Where-Object { $_.Id -eq $live.Id })
        if ($seen.Count -ne 1) { throw "live headless child $($live.Id) missing from active list" }
        $dead = Start-Process -FilePath $hostPath -ArgumentList @(
            "-NoProfile", "-Command", "exit 0") -PassThru -WindowStyle Hidden
        $deadExit = $dead.WaitForExit(10000)
        if (-not $deadExit) { throw "exited child $($dead.Id) did not terminate" }
        Start-Sleep -Milliseconds 300
        $seenAfter = @(Get-ActiveVita3KProcess -Name $hostName | Where-Object { $_.Id -eq $dead.Id })
        if ($seenAfter.Count -ne 0) { throw "exited child $($dead.Id) leaked into active list" }
        $rawVita = @(Get-Process -Name "Vita3K" -ErrorAction SilentlyContinue)
        $staleVita = @($rawVita | Where-Object { $_.HasExited })
        $activeVita = @(Get-ActiveVita3KProcess -Name "Vita3K")
        foreach ($s in $staleVita) {
            if (@($activeVita | Where-Object { $_.Id -eq $s.Id }).Count -ne 0) {
                throw "stale Vita3K process $($s.Id) was reported active"
            }
        }
        Write-Output ("PROCESS_CHECK_SELFTEST PASS live_id={0} dead_id={1} stale_vita=[{2}] active_vita=[{3}] skipped=[{4}]" -f `
            $live.Id, $dead.Id, (($staleVita | ForEach-Object { $_.Id }) -join ","), `
            (($activeVita | ForEach-Object { $_.Id }) -join ","), `
            (($script:Vita3KSkippedEntries | ForEach-Object { $_.Id }) -join ","))
    } catch {
        Write-Error ("PROCESS_CHECK_SELFTEST FAIL: {0} [{1}] at {2}" -f $_.Exception.Message, $_.Exception.GetType().FullName, $_.ScriptStackTrace)
        $failed = $true
    } finally {
        if ($live -and -not $live.HasExited) { Stop-Process -Id $live.Id -Force -ErrorAction SilentlyContinue }
        if ($live) { $live.Dispose() }
        if ($dead) { $dead.Dispose() }
    }
    if ($failed) { exit 1 } else { exit 0 }
}

$Root = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
if (-not $Config) {
    $Config = Join-Path $Root "config\vita3k-play.yml"
}
if (-not $OutDir) {
    $OutDir = Join-Path $Root ("build\runs\play-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$Config = (Resolve-Path $Config).Path
if (-not $Emulator) { $Emulator = & "$PSScriptRoot/prepare_vita3k_runtime.ps1" }
$Exe = (Resolve-Path $Emulator).Path
$script:Vita3KSkippedEntries = @()
$activeVita3K = @(Get-ActiveVita3KProcess -Name "Vita3K")
if ($activeVita3K.Count -gt 0) {
    $ids = (($activeVita3K | ForEach-Object {
        "PID $($_.Id) exited=$($_.HasExited) exe=$($_.ExecutablePath) hwnd=$($_.MainWindowHandle)"
    }) -join "; ")
    throw "Vita3K is already running. Close it before changing the shared installation or play markers. $ids"
}
if ($script:Vita3KSkippedEntries.Count -gt 0) {
    Write-Output ("Ignoring terminated Vita3K process entries: " +
        (($script:Vita3KSkippedEntries | ForEach-Object { "PID $($_.Id)" }) -join ", "))
}
$OutDir = (Resolve-Path $OutDir).Path
$utf8 = New-Object System.Text.UTF8Encoding($false)
$pref = Join-Path $Root 'build\vita3k-pref'
$data = Join-Path $pref 'ux0\data\mhgu-vita-port'
$app = Join-Path $pref 'ux0\app\MHGUVP018'
$staticShaders = "$data\static-shaders"
$legacyShaders = "$data\shaders"
# B108: validate the tracked render-resolution selection before any marker or
# installation mutation. The native runtime reads only the staged data copy,
# once at GXM startup; this runner makes the repository file authoritative for
# the next launch. An unsupported value must fail here, never fall back.
$renderResolutionSource = Join-Path $Root 'config\render-resolution.txt'
if (-not (Test-Path -LiteralPath $renderResolutionSource -PathType Leaf)) {
    throw "Render resolution config is missing: $renderResolutionSource"
}
$renderResolutionRaw = [IO.File]::ReadAllText($renderResolutionSource)
$renderResolutionMode = $renderResolutionRaw.TrimEnd([char[]]" `t`r`n")
if ($renderResolutionMode -cnotin @('360p', '540p')) {
    throw ("Unsupported render resolution '$renderResolutionMode' in " +
        "$renderResolutionSource; expected 360p or 540p")
}
$renderResolutionDest = Join-Path $data 'render-resolution.txt'
# An explicit caller source wins. Otherwise use the locally selected package,
# never infer a source from the destination compiled into the SELF.
if (-not $StaticShaderSource) {
    $runtimeSelectionPath = Join-Path $Root 'config/vita3k-runtime.json'
    if (Test-Path -LiteralPath $runtimeSelectionPath) {
        $runtimeSelection = Get-Content -LiteralPath $runtimeSelectionPath -Raw | ConvertFrom-Json
        if ($runtimeSelection.static_shader_source) {
            $selectedSource = [string]$runtimeSelection.static_shader_source
            if (-not [IO.Path]::IsPathRooted($selectedSource)) {
                $selectedSource = Join-Path $Root $selectedSource
            }
            $StaticShaderSource = (Resolve-Path -LiteralPath $selectedSource).Path
            if (-not $StaticResourceSidecar) {
                $StaticResourceSidecar = Join-Path $StaticShaderSource 'shader-resources.bin'
            }
            if (-not $StaticResourceManifest) {
                $StaticResourceManifest = Join-Path $StaticShaderSource 'shader-resources.manifest.json'
            }
        }
    }
}
if ($StaticShaderSource) {
    if (-not [IO.Path]::IsPathRooted($StaticShaderSource)) {
        throw "StaticShaderSource must be an absolute path: $StaticShaderSource"
    }
    if (-not (Test-Path -LiteralPath $StaticShaderSource -PathType Container)) {
        throw "StaticShaderSource directory is missing: $StaticShaderSource"
    }
    $sourceBank = (Resolve-Path -LiteralPath $StaticShaderSource).Path
} else {
    $sourceBank = (Resolve-Path -LiteralPath $staticShaders).Path
}
if (-not (Test-Path "$sourceBank\shader-index.bin") -and -not (Test-Path "$legacyShaders\index.txt")) {
    throw "Shader bundle missing: $sourceBank\shader-index.bin or $legacyShaders\index.txt"
}
if (-not $Vpk) { $Vpk = Join-Path $Root 'build\vita\vita_port_native_loader.vpk' }
$Vpk = (Resolve-Path $Vpk).Path
if ($Replay) { $Replay = (Resolve-Path $Replay).Path }

# Keep diagnostic configurations separate from the reproducible play entry.
# In particular, no display marker leaves Vita3K's completed 100% overlay on
# screen forever although the guest continues running. Never hide the overlay
# or erase the shader cache to conceal this missing display submission.
. (Join-Path $PSScriptRoot 'renderer_policy.ps1')
. (Join-Path $PSScriptRoot 'window_close.ps1')
$cfg = Get-Content -LiteralPath $Config -Raw
$requestedRenderer = Get-RendererFromConfig $cfg
if ($CaptureModels -or $SurfaceReadback) {
    # A CPU surface dump is meaningful only after an actual GPU readback.
    $cfg = [regex]::Replace($cfg, '(?m)^disable-surface-sync:.*$', 'disable-surface-sync: false')
}
$cfg = [regex]::Replace($cfg, '(?m)^pref-path:.*$', ('pref-path: ' + ($pref.Replace('\','/') + '/')))
$runConfig = Join-Path $OutDir 'config.yml'
[IO.File]::WriteAllText($runConfig, $cfg, $utf8)
$Config = $runConfig
# Vita3K's merge omits fields equal to built-in defaults. Its base config must
# therefore agree with the requested config; only write our private runtime.
$runtimeRoot=Split-Path $Exe
$privateRoot=[IO.Path]::GetFullPath((Join-Path $Root 'build/vita3k-runtime'))
if ($runtimeRoot -eq $privateRoot) {
    [IO.File]::WriteAllText((Join-Path $runtimeRoot 'config.yml'),$cfg,$utf8)
}
Assert-RendererBaseConfig $runtimeRoot $requestedRenderer
# B48: record and enforce the audio adapter identity. SDL is Vita3K's built-in
# default, so a Cubeb value in the executable-directory base config would win
# over an SDL request during merge; both files must agree.
$requestedAudioBackend = Get-AudioBackendFromConfig $cfg
$baseAudioBackend = Assert-AudioBackendBaseConfig $runtimeRoot $requestedAudioBackend
# B108: stage the validated selection into the shared data directory before
# process launch, including when StaticShaderSource was explicit. Copying the
# exact source bytes preserves the trailing newline the runtime tolerates. The
# output copy is the per-run receipt for the selected text.
New-Item -ItemType Directory -Force -Path $data | Out-Null
Copy-Item -LiteralPath $renderResolutionSource -Destination $renderResolutionDest -Force
Copy-Item -LiteralPath $renderResolutionSource -Destination (Join-Path $OutDir 'render-resolution.txt') -Force
# Bounded batching is an explicit experimental opt-in. Snapshot both markers and
# restore them byte-for-byte in the final cleanup so an interrupted or switched
# run cannot leave the experimental path enabled.
# clear-fusion.enable/.disable are intentionally not snapshotted, moved or
# deleted: the runtime gives .disable precedence over .enable and the default,
# and a user's persistent opt-out must survive every run.
$boundedBatchNames = @('bounded-batch.enable', 'bounded-batch.disable')
$boundedBatchSaved = @{}
foreach ($name in $boundedBatchNames) {
    $markerPath = Join-Path $data $name
    if (Test-Path -LiteralPath $markerPath) {
        $boundedBatchSaved[$name] = [IO.File]::ReadAllBytes($markerPath)
        Remove-Item -LiteralPath $markerPath -Force
    }
}
if ($BoundedBatch) {
    [IO.File]::WriteAllText((Join-Path $data 'bounded-batch.enable'), '', $utf8)
}
if (-not $DiagnosticMode) {
    $disabled = Join-Path $OutDir 'disabled-markers'
    New-Item -ItemType Directory -Force $disabled | Out-Null
    # Do not add clear-fusion.disable here: moving it would defeat the user's
    # persistent opt-out for the duration of the run.
    $conflicts = @('display-bridge.disable','gxm-test.enable','m2-frame.enable',
        'm3-drawdump.enable','m3-viewport.enable','audio-probe.enable',
        'audio-probe-monitor.enable','audio-command-dump.enable','swkbd-selftest.enable',
        'hid-helper-probe.enable','scene-probe.enable','media-instrument.enable',
        'f8-state-override.enable','f8-probe.enable',
        'input-replay.txt','applet-exit','applet-exit-delay-ms',
        'gpu-present-bottom-origin.enable','input-live.enable','input-live.txt',
        'play-perf.enable','capture-live.enable','capture-request.txt')
    foreach ($name in $conflicts) {
        if (Test-Path -LiteralPath (Join-Path $data $name)) {
            Move-Item -LiteralPath (Join-Path $data $name) -Destination (Join-Path $disabled $name)
        }
    }
    foreach ($name in @('play.enable','display-bridge.enable','media-decode.enable','media-video.enable','gpu-present.enable')) {
        [IO.File]::WriteAllText((Join-Path $data $name), '', $utf8)
    }
    if ($LiveInput) {
        [IO.File]::WriteAllText((Join-Path $data 'input-live.enable'), '', $utf8)
        [IO.File]::WriteAllText((Join-Path $data 'input-live.txt'), "0 0 0 0 0`n", $utf8)
    }
    if ($PlayPerf) {
        [IO.File]::WriteAllText((Join-Path $data 'play-perf.enable'), '', $utf8)
    }
    if ($LiveCapture) {
        # Keep the idle mailbox present: Vita3K logs each failed open/stat.
        [IO.File]::WriteAllText((Join-Path $data 'capture-request.txt'), "0 0`n", $utf8)
        [IO.File]::WriteAllText((Join-Path $data 'capture-live.enable'), '', $utf8)
    }
    if ($CaptureShaders) {
        if (Test-Path "$data/shader-word-dump.disable") {
            Move-Item -LiteralPath "$data/shader-word-dump.disable" -Destination "$disabled/shader-word-dump.disable"
        }
    } else { [IO.File]::WriteAllText("$data/shader-word-dump.disable", '', $utf8) }
    if ($Replay) { Copy-Item -LiteralPath $Replay -Destination "$data/input-replay.txt" }
    $offlineSystemData = Join-Path $data 'offline-ngword2.romfs'
    if (-not (Test-Path -LiteralPath $offlineSystemData -PathType Leaf)) {
        $python=if ($env:MHGU_PYTHON) { $env:MHGU_PYTHON } else { (Get-Command python -ErrorAction Stop).Source }
        & $python "$PSScriptRoot/../romfs/build_offline_system_data.py" --output $offlineSystemData
        if ($LASTEXITCODE -ne 0) { throw 'Offline system data generation failed; run scripts/prepare-runtime.ps1 first.' }
    }
} elseif (Test-Path "$data/play.enable") {
    # A diagnostic launch must retain the historical bounded-stop semantics.
    Move-Item -LiteralPath "$data/play.enable" -Destination "$OutDir/play.enable.saved"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($Vpk)
try {
    foreach ($entry in $zip.Entries) {
        $dest = [IO.Path]::GetFullPath((Join-Path $app $entry.FullName))
        if (-not $dest.StartsWith($app + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'VPK traversal rejected'
        }
        if ($entry.FullName.EndsWith('/')) { New-Item -ItemType Directory -Force $dest | Out-Null; continue }
        New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($dest)) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest, $true)
    }
    if ($CaptureModels) {
        [IO.File]::WriteAllText((Join-Path $data 'f8-probe.enable'), '', $utf8)
    }
    if ($CaptureAudio) {
        $previousAudio = Join-Path $OutDir 'previous-audio'
        New-Item -ItemType Directory -Force $previousAudio | Out-Null
        foreach ($file in (Get-ChildItem -LiteralPath $data -File -Filter 'audio-*.bin')) {
            Move-Item -LiteralPath $file.FullName -Destination (Join-Path $previousAudio $file.Name)
        }
        [IO.File]::WriteAllText((Join-Path $data 'audio-command-dump.enable'), '', $utf8)
    }
    $stream = $zip.GetEntry('eboot.bin').Open()
    try {
        $sha = [Security.Cryptography.SHA256]::Create()
        try { $expected = [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-','') }
        finally { $sha.Dispose() }
    } finally { $stream.Dispose() }
} finally { $zip.Dispose() }
if ((Get-FileHash -LiteralPath "$app/eboot.bin").Hash -ne $expected) { throw 'Installed eboot hash mismatch' }
$record = [ordered]@{requested_backend=$requestedRenderer;title_id='MHGUVP018';config=$Config;pref=$pref;diagnostic=[bool]$DiagnosticMode;
    eboot_sha256=$expected;vpk_sha256=(Get-FileHash $Vpk).Hash;
    emulator_sha256=(Get-FileHash $Exe).Hash;replay=$Replay;silent_audio=[bool]$SilentAudio;
    requested_audio_backend=$requestedAudioBackend;base_audio_backend=$baseAudioBackend;
    capture_models=[bool]$CaptureModels;capture_audio=[bool]$CaptureAudio;play_perf=[bool]$PlayPerf;
    live_input=[bool]$LiveInput;live_capture=[bool]$LiveCapture;
    capture_schedule=@($captureSchedule);capture_schedule_dropped=@($captureScheduleInfo.Dropped);
    capture_schedule_normalized=[bool]$captureScheduleInfo.Normalized;
    capture_skipped_reason=$(if ($NoWindowCapture) { 'no-window-capture' } else { '' });
    capture_ready_timeout_seconds=[int]$CaptureReadyTimeoutSeconds;
    render_resolution=[ordered]@{mode=$renderResolutionMode;
        source=$renderResolutionSource;staged=$renderResolutionDest;
        text=$renderResolutionRaw};
    dpi_awareness=$script:DpiAwareness}
$record.sources = [ordered]@{}
foreach ($name in @('src/platform/guest_svc.c','src/platform/guest_svc.h',
                    'src/platform/guest_storage.c','src/platform/guest_storage.h',
                    'src/platform/nvmap_table.c','src/platform/nvmap_table.h',
                    'src/platform/memory_attributes.c','src/platform/memory_attributes.h',
                    'src/platform/time_calendar.h','src/platform/gxm_runtime.c',
                    'src/platform/frame_cost_profile.c','src/platform/frame_cost_profile.h',
                    'src/platform/texture_pool.c','src/platform/texture_pool.h',
                    'src/bridge/armv7/sdk_dso.S',
                    'src/platform/log_sink.c','src/platform/log_sink.h',
                    'src/platform/vertex_format.c','src/platform/vertex_format.h',
                    'src/platform/vertex_window.c','src/platform/vertex_window.h',
                    'src/platform/gpu_indices.h',
                    'src/platform/gpu_texture.c','src/platform/gpu_texture.h',
                    'src/platform/gpu_stream.c','src/platform/gpu_stream.h',
                    'src/platform/gxm_runtime.h','tools/shader/shader_ir.py',
                    'src/platform/gxm_resource_layout.c','src/platform/gxm_resource_layout.h',
                    'src/platform/gxm_depth_view.c','src/platform/gxm_depth_view.h',
                    'src/platform/gxm_depth_view_native.c','src/platform/gxm_depth_view_native.h',
                    'src/platform/gxm_depth_clear_native.c','src/platform/gxm_depth_clear_native.h',
                    'tools/shader/build_resource_layout.py',
                    'tools/shader/translate_captured.py','config/graphics/character-input-types.json',
                    'src/platform/audio_render.c','src/platform/audio_render.h','src/platform/audio_downmix.h',
                    'src/platform/audio_ipc.c','src/platform/audio_ipc.h',
                    'src/platform/media_decode.c','src/platform/media_decode.h',
                    'src/platform/media_video.c','src/platform/media_video.h',
                    'src/loader/sdk_dso.c','src/app/main.c',
                    'tools/runtime/run_gui_play.ps1','tools/runtime/observe_process_lifecycle.ps1','tools/runtime/detached_process.ps1',
                    'config/vita3k-play.yml')) {
    $record.sources[$name] = (Get-FileHash -LiteralPath (Join-Path $Root $name)).Hash
}
$record.shaders=[ordered]@{}
if (Test-Path "$sourceBank\shader-index.bin") {
    $record.shader_mode='static-index-v1'
    $sourceIndexPath = Join-Path $sourceBank 'shader-index.bin'
    $sourceIndexJsonPath = Join-Path $sourceBank 'shader-index.json'
    $sourceManifestPath = Join-Path $sourceBank 'manifest.json'
    foreach ($required in @($sourceIndexPath, $sourceIndexJsonPath, $sourceManifestPath)) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "Static shader source is missing $required"
        }
    }
    $indexHeader = [IO.File]::ReadAllBytes($sourceIndexPath)
    if ($indexHeader.Length -lt 64 -or
        [Text.Encoding]::ASCII.GetString($indexHeader, 0, 7) -ne 'VPSHIX1') {
        throw 'Static shader index header/magic rejected'
    }
    $sourceAbi = [BitConverter]::ToUInt32($indexHeader, 48)
    $sourceStages = [BitConverter]::ToUInt32($indexHeader, 16)
    $sourceVariants = [BitConverter]::ToUInt32($indexHeader, 20)
    $sourcePrograms = [BitConverter]::ToUInt32($indexHeader, 24)
    if ($sourceAbi -ne 2 -or $sourceStages -eq 0 -or $sourceVariants -eq 0 -or $sourcePrograms -eq 0) {
        throw "Static shader index ABI/counts rejected: abi=$sourceAbi stages=$sourceStages variants=$sourceVariants programs=$sourcePrograms"
    }
    $record.static_shader_index_sha256=(Get-FileHash -LiteralPath $sourceIndexPath).Hash.ToLowerInvariant()
    $record.static_shader_abi_version = $sourceAbi
    $record.static_shader_stage_count = $sourceStages
    $record.static_shader_variant_count = $sourceVariants
    $record.static_shader_program_count = $sourcePrograms
    $record.static_shader_index_crc32_full = ('{0:x8}' -f [VPIndexCrc32]::Compute($indexHeader))
    $record.static_shader_index_body_crc32 = ('{0:x8}' -f [BitConverter]::ToUInt32($indexHeader, 44))
    $record.static_shader_source = $sourceBank
    $record.static_shader_manifest_sha256=(Get-FileHash -LiteralPath $sourceManifestPath).Hash.ToLowerInvariant()
    $sourceManifest = Get-Content -LiteralPath $sourceManifestPath -Raw | ConvertFrom-Json
    if ([string]$sourceManifest.index_sha256 -ne $record.static_shader_index_sha256) {
        throw 'Static shader source manifest index SHA differs from shader-index.bin'
    }
    if ($null -ne $sourceManifest.abi_version -and [int]$sourceManifest.abi_version -ne [int]$sourceAbi) {
        throw 'Static shader source manifest ABI differs from the index header'
    }
    if ($null -eq $sourceManifest.candidate_counts -or
        [uint64]$sourceManifest.candidate_counts.stages -ne [uint64]$sourceStages -or
        [uint64]$sourceManifest.candidate_counts.variants -ne [uint64]$sourceVariants -or
        [uint64]$sourceManifest.candidate_counts.programs -ne [uint64]$sourcePrograms) {
        throw 'Static shader source manifest candidate counts differ from the index header'
    }
    $sourceIndexJsonSha=(Get-FileHash -LiteralPath $sourceIndexJsonPath).Hash.ToLowerInvariant()
    if ([string]$sourceManifest.index_json_sha256 -ne $sourceIndexJsonSha) {
        throw 'Static shader source manifest index JSON SHA differs from shader-index.json'
    }
    $sourceIndexJson = Get-Content -LiteralPath $sourceIndexJsonPath -Raw | ConvertFrom-Json
    if ($sourceIndexJson.variants.Count -ne $sourceVariants -or
        $sourceIndexJson.stages.Count -ne $sourceStages -or
        $sourceIndexJson.programs.Count -ne $sourcePrograms) {
        throw 'Static shader source index JSON counts differ from the binary header'
    }
    # Validate every GXP below the explicit source directory, including its
    # size, SHA-256 and full-file CRC32 from shader-index.json.  The copied
    # package must contain exactly this set, never a leftover file.
    $expectedShaderNames = New-Object 'System.Collections.Generic.HashSet[string]'
    $sourceGxpBytes = [uint64]0
    foreach ($variant in $sourceIndexJson.variants) {
        $key = [string]$variant.key
        if ($key -notmatch '^[0-9a-f]{32}$') { throw "Static shader variant key rejected: $key" }
        $relative = ([string]$variant.gxp).Replace('/','\')
        if ($relative -notmatch '^shaders\\[0-9a-f]{32}\.gxp$' -or
            [IO.Path]::GetFileName($relative) -ne ($key + '.gxp')) {
            throw "Static shader GXP path is not the exact key file: $($variant.gxp)"
        }
        $gxpPath = [IO.Path]::GetFullPath((Join-Path $sourceBank $relative))
        if (-not $gxpPath.StartsWith($sourceBank + [IO.Path]::DirectorySeparatorChar,
                [StringComparison]::OrdinalIgnoreCase)) {
            throw "Static shader GXP escapes its source bank: $($variant.gxp)"
        }
        if (-not (Test-Path -LiteralPath $gxpPath -PathType Leaf)) {
            throw "Static shader GXP is missing: $gxpPath"
        }
        $gxpLength = (Get-Item -LiteralPath $gxpPath).Length
        if ([uint64]$gxpLength -ne [uint64]$variant.gxp_bytes) {
            throw "Static shader GXP size differs from index JSON: $gxpPath"
        }
        $gxpSha = (Get-FileHash -LiteralPath $gxpPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($gxpSha -ne [string]$variant.gxp_sha256) {
            throw "Static shader GXP SHA differs from index JSON: $gxpPath"
        }
        $gxpBytes = [IO.File]::ReadAllBytes($gxpPath)
        if ([uint64][VPIndexCrc32]::Compute($gxpBytes) -ne [uint64]$variant.gxp_crc32) {
            throw "Static shader GXP CRC differs from index JSON: $gxpPath"
        }
        if (-not $expectedShaderNames.Add([IO.Path]::GetFileName($gxpPath))) {
            throw "Duplicate static shader GXP record: $gxpPath"
        }
        $sourceGxpBytes += [uint64]$gxpLength
    }
    $sourceGxpFiles = @(Get-ChildItem -LiteralPath (Join-Path $sourceBank 'shaders') -Filter '*.gxp' -File)
    if ($sourceGxpFiles.Count -ne $sourceVariants) {
        throw "Static shader source GXP count $($sourceGxpFiles.Count) differs from index variants $sourceVariants"
    }
    foreach ($gxpFile in $sourceGxpFiles) {
        if (-not $expectedShaderNames.Contains($gxpFile.Name)) {
            throw "Static shader source contains a GXP outside the index: $($gxpFile.Name)"
        }
    }
    $allowedSourceFiles = @('candidate-spec.json','manifest.json','shader-index.bin',
        'shader-index.json','shader-resources.bin','shader-resources.manifest.json')
    foreach ($sourceFile in @(Get-ChildItem -LiteralPath $sourceBank -File)) {
        if ($sourceFile.Name -notin $allowedSourceFiles) {
            throw "Static shader source contains an unexpected file: $($sourceFile.Name)"
        }
    }
    foreach ($sourceDir in @(Get-ChildItem -LiteralPath $sourceBank -Directory)) {
        if ($sourceDir.Name -ne 'shaders') {
            throw "Static shader source contains an unexpected directory: $($sourceDir.Name)"
        }
    }
    $record.static_shader_source_gxp_bytes = $sourceGxpBytes
    # The frozen/source bank is never modified. The SELF is compiled with an
    # explicit package directory; this runner stages only this source bank and
    # the requested VPSRES1 sidecar into that separate child directory.
    $buildPackageMetaPath = Join-Path (Split-Path -Parent $Vpk) 'static-shader-package.json'
    if (-not (Test-Path -LiteralPath $buildPackageMetaPath)) {
        throw "Build metadata missing: $buildPackageMetaPath (rebuild the SELF)"
    }
    $buildPackageMeta = Get-Content -LiteralPath $buildPackageMetaPath -Raw | ConvertFrom-Json
    $compiledRuntimeDir = [string]$buildPackageMeta.runtime_dir
    $runtimeMatch = [regex]::Match($compiledRuntimeDir,
        '^ux0:data/mhgu-vita-port/([A-Za-z0-9._-]+)$')
    if (-not $runtimeMatch.Success) {
        throw "Compiled static shader package path is not a validated child: $compiledRuntimeDir"
    }
    $packageLeaf = $runtimeMatch.Groups[1].Value
    if ($packageLeaf -eq 'static-shaders' -or $packageLeaf -eq 'shaders') {
        throw "Compiled static shader package must not be the frozen/legacy bank: $packageLeaf"
    }
    if ($StaticResourcePackageDir) {
        $requestedRuntimeDir = $StaticResourcePackageDir.TrimEnd('/','\')
        if ($requestedRuntimeDir -notmatch '^ux0:data/mhgu-vita-port/([A-Za-z0-9._-]+)$') {
            throw "StaticResourcePackageDir must be ux0:data/mhgu-vita-port/<leaf>: $StaticResourcePackageDir"
        }
        if ($requestedRuntimeDir -ne $compiledRuntimeDir) {
            throw "StaticResourcePackageDir $requestedRuntimeDir does not match the SELF build package $compiledRuntimeDir"
        }
    }
    $resourcePackage = Join-Path $data $packageLeaf
    if (-not $StaticResourceSidecar) {
        $StaticResourceSidecar = Join-Path $Root 'build\static-shader-package\shader-resources.bin'
    }
    if (-not $StaticResourceManifest) {
        $StaticResourceManifest = Join-Path $Root 'build\static-shader-package\shader-resources.manifest.json'
    }
    $resourceSidecarSource = (Resolve-Path -LiteralPath $StaticResourceSidecar).Path
    $resourceManifestSource = (Resolve-Path -LiteralPath $StaticResourceManifest).Path
    $resourceMeta = Get-Content -LiteralPath $resourceManifestSource -Raw | ConvertFrom-Json
    if ([string]$resourceMeta.index.sha256 -ne $record.static_shader_index_sha256.ToLowerInvariant()) {
        throw 'Resource sidecar manifest parent index SHA differs from the selected source bank index'
    }
    if ([uint64]$resourceMeta.index.stages -ne [uint64]$sourceStages -or
        [uint64]$resourceMeta.index.variants -ne [uint64]$sourceVariants -or
        [uint64]$resourceMeta.index.programs -ne [uint64]$sourcePrograms -or
        [int]$resourceMeta.index.abi -ne [int]$sourceAbi) {
        throw 'Resource sidecar manifest counts/ABI differ from the selected source bank'
    }
    $expectedIndexCrc = [VPIndexCrc32]::Compute($indexHeader)
    $manifestIndexCrc = if ($resourceMeta.index.crc32 -is [string]) {
        [Convert]::ToUInt32([string]$resourceMeta.index.crc32, 16)
    } else {
        [uint64]$resourceMeta.index.crc32
    }
    if ([uint64]$manifestIndexCrc -ne [uint64]$expectedIndexCrc) {
        throw 'Resource sidecar manifest full index CRC differs from the selected source bank index'
    }
    $sidecarBytes = [IO.File]::ReadAllBytes($resourceSidecarSource)
    if ($sidecarBytes.Length -lt 96 -or
        [Text.Encoding]::ASCII.GetString($sidecarBytes, 0, 7) -ne 'VPSRES1' -or
        $sidecarBytes[7] -ne 0) {
        throw 'Resource sidecar magic/header rejected'
    }
    if ([BitConverter]::ToUInt16($sidecarBytes, 8) -ne 1 -or
        [BitConverter]::ToUInt16($sidecarBytes, 10) -ne 96 -or
        [BitConverter]::ToUInt32($sidecarBytes, 12) -ne 1) {
        throw 'Resource sidecar version/header/flags rejected'
    }
    $sidecarParentSha = -join ($sidecarBytes[40..71] | ForEach-Object { $_.ToString('x2') })
    $sidecarParentCrc = [BitConverter]::ToUInt32($sidecarBytes, 72)
    if ($sidecarParentSha -ne $record.static_shader_index_sha256.ToLowerInvariant() -or
        $sidecarParentCrc -ne $expectedIndexCrc) {
        throw 'Resource sidecar full parent index identity differs from the selected source bank index'
    }
    $resourceSidecarSourceHash = (Get-FileHash -LiteralPath $resourceSidecarSource).Hash.ToLowerInvariant()
    if ([string]$resourceMeta.sidecar.sha256 -ne $resourceSidecarSourceHash -or
        [uint64]$resourceMeta.sidecar.bytes -ne [uint64]$sidecarBytes.Length) {
        throw 'Resource sidecar bytes differ from their manifest identity'
    }
    $sourceSidecarPath = Join-Path $sourceBank 'shader-resources.bin'
    if ((Test-Path -LiteralPath $sourceSidecarPath -PathType Leaf) -and
        (Get-FileHash -LiteralPath $sourceSidecarPath).Hash.ToLowerInvariant() -ne $resourceSidecarSourceHash) {
        throw 'Explicit resource sidecar differs from the selected source bank sidecar'
    }
    $sourceGxpCount = $sourceGxpFiles.Count
    $packageIndexPath = Join-Path $resourcePackage 'shader-index.bin'
    $packageSidecarPath = Join-Path $resourcePackage 'shader-resources.bin'
    if (Test-Path -LiteralPath $resourcePackage) {
        if (-not (Test-Path -LiteralPath $packageIndexPath -PathType Leaf) -or
            -not (Test-Path -LiteralPath $packageSidecarPath -PathType Leaf)) {
            throw "Existing resource package is incomplete; refusing to overwrite: $resourcePackage"
        }
        $destIndexHash = (Get-FileHash -LiteralPath $packageIndexPath).Hash.ToLowerInvariant()
        $destSidecarHash = (Get-FileHash -LiteralPath $packageSidecarPath).Hash.ToLowerInvariant()
        $destShaders = @(Get-ChildItem -LiteralPath (Join-Path $resourcePackage 'shaders') -Filter '*.gxp' -File)
        $packageMatches = ($destIndexHash -eq $record.static_shader_index_sha256.ToLowerInvariant() -and
            $destSidecarHash -eq $resourceSidecarSourceHash -and
            $destShaders.Count -eq $sourceGxpCount)
        if ($packageMatches) {
            foreach ($gxpFile in $sourceGxpFiles) {
                $destFile = Join-Path (Join-Path $resourcePackage 'shaders') $gxpFile.Name
                if (-not (Test-Path -LiteralPath $destFile -PathType Leaf) -or
                    (Get-FileHash -LiteralPath $destFile).Hash.ToLowerInvariant() -ne
                        (Get-FileHash -LiteralPath $gxpFile.FullName).Hash.ToLowerInvariant()) {
                    $packageMatches = $false
                    break
                }
            }
        }
        if (-not $packageMatches) {
            throw "Existing resource package identity differs from the selected source; refusing to overwrite: $resourcePackage"
        }
    } else {
        $stageLeaf = '_b29-package-stage-' + [guid]::NewGuid().ToString('N')
        $stagePackage = Join-Path $data $stageLeaf
        New-Item -ItemType Directory -Force -Path (Join-Path $stagePackage 'shaders') | Out-Null
        try {
            foreach ($name in @('candidate-spec.json','manifest.json','shader-index.bin','shader-index.json')) {
                $sourceFile = Join-Path $sourceBank $name
                if (Test-Path -LiteralPath $sourceFile -PathType Leaf) {
                    Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $stagePackage $name) -Force
                }
            }
            foreach ($gxpFile in $sourceGxpFiles) {
                Copy-Item -LiteralPath $gxpFile.FullName -Destination (Join-Path $stagePackage ('shaders\' + $gxpFile.Name)) -Force
            }
            Copy-Item -LiteralPath $resourceSidecarSource -Destination (Join-Path $stagePackage 'shader-resources.bin') -Force
            Copy-Item -LiteralPath $resourceManifestSource -Destination (Join-Path $stagePackage 'shader-resources.manifest.json') -Force
            $stagedIndexHash = (Get-FileHash -LiteralPath (Join-Path $stagePackage 'shader-index.bin')).Hash.ToLowerInvariant()
            $stagedSidecarHash = (Get-FileHash -LiteralPath (Join-Path $stagePackage 'shader-resources.bin')).Hash.ToLowerInvariant()
            $stagedShaders = @(Get-ChildItem -LiteralPath (Join-Path $stagePackage 'shaders') -Filter '*.gxp' -File)
            if ($stagedIndexHash -ne $record.static_shader_index_sha256.ToLowerInvariant() -or
                $stagedSidecarHash -ne $resourceSidecarSourceHash -or
                $stagedShaders.Count -ne $sourceGxpCount) {
                throw 'Staged resource package index/sidecar/GXP count differs from the source identity'
            }
            foreach ($gxpFile in $sourceGxpFiles) {
                $stagedFile = Join-Path (Join-Path $stagePackage 'shaders') $gxpFile.Name
                if (-not (Test-Path -LiteralPath $stagedFile -PathType Leaf) -or
                    (Get-FileHash -LiteralPath $stagedFile).Hash.ToLowerInvariant() -ne
                        (Get-FileHash -LiteralPath $gxpFile.FullName).Hash.ToLowerInvariant()) {
                    throw "Staged GXP identity differs from the source: $($gxpFile.Name)"
                }
            }
            Move-Item -LiteralPath $stagePackage -Destination $resourcePackage
        } finally {
            if (Test-Path -LiteralPath $stagePackage) {
                Remove-Item -LiteralPath $stagePackage -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }
    $resourceSidecarHash = (Get-FileHash -LiteralPath $packageSidecarPath).Hash.ToLowerInvariant()
    $resourceIndexHash = (Get-FileHash -LiteralPath $packageIndexPath).Hash.ToLowerInvariant()
    if ($resourceIndexHash -ne $record.static_shader_index_sha256.ToLowerInvariant() -or
        $resourceSidecarHash -ne $resourceSidecarSourceHash) {
        throw 'Resource package copy differs from the verified source identity'
    }
    $resourceGxpCount = @(Get-ChildItem -LiteralPath (Join-Path $resourcePackage 'shaders') -Filter '*.gxp' -File).Count
    if ($resourceGxpCount -ne $sourceGxpCount) {
        throw "Resource package GXP count $resourceGxpCount differs from source bank $sourceGxpCount"
    }
    $packageManifest = [ordered]@{
        schema = 1
        package_id = ('static-vpsres1-' + $resourceSidecarHash.Substring(0,16))
        active = $true
        runtime_dir = $compiledRuntimeDir
        built_self_meta = $buildPackageMetaPath
        built_self_meta_sha256 = (Get-FileHash -LiteralPath $buildPackageMetaPath).Hash.ToLowerInvariant()
        source_bank = $sourceBank
        source_bank_index_sha256 = $record.static_shader_index_sha256
        source_bank_manifest_sha256 = $record.static_shader_manifest_sha256
        source_sidecar = $resourceSidecarSource
        source_sidecar_manifest = $resourceManifestSource
        index = [ordered]@{
            sha256 = $resourceIndexHash
            crc32 = ('{0:x8}' -f $expectedIndexCrc)
            crc32_scope = 'entire shader-index.bin'
            body_crc32 = ('{0:x8}' -f [BitConverter]::ToUInt32($indexHeader, 44))
            body_crc32_scope = 'index body only; not the sidecar parent identity'
            stages = $resourceMeta.index.stages
            variants = $resourceMeta.index.variants
            programs = $resourceMeta.index.programs
            abi = $resourceMeta.index.abi
        }
        sidecar = [ordered]@{
            sha256 = $resourceSidecarHash
            bytes = (Get-Item -LiteralPath (Join-Path $resourcePackage 'shader-resources.bin')).Length
            stage_records = $resourceMeta.sidecar.stage_records
            texture_records = $resourceMeta.sidecar.texture_records
        }
        gxp = [ordered]@{ files = $resourceGxpCount; source_files = $sourceGxpCount; source_bytes = $sourceGxpBytes; bytes_unchanged_from_source_bank = $true }
        limitations = @(
            'This directory is a generated copy; the explicit source bank is not rewritten.',
            'The sidecar is accepted only with the exact parent index SHA-256 and CRC32.',
            'The active runtime directory is the directory compiled into this SELF.',
            'Package identity is not proof of GPU execution, pixels or hardware behaviour.'
        )
    }
    [IO.File]::WriteAllText((Join-Path $resourcePackage 'package-manifest.json'),
        ($packageManifest | ConvertTo-Json -Depth 6), $utf8)
    $record.static_shader_resource_sidecar_sha256 = $resourceSidecarHash
    $record.static_shader_runtime_dir = $compiledRuntimeDir
    $record.static_resource_package = [ordered]@{
        path = $resourcePackage
        runtime_dir = $compiledRuntimeDir
        package_id = $packageManifest.package_id
        active = $true
        index_sha256 = $resourceIndexHash
        sidecar_sha256 = $resourceSidecarHash
        gxp_files = $resourceGxpCount
        source_bank = $sourceBank
        source_bank_index_sha256 = $record.static_shader_index_sha256
        source_bank_manifest_sha256 = $record.static_shader_manifest_sha256
        manifest_sha256 = (Get-FileHash -LiteralPath (Join-Path $resourcePackage 'package-manifest.json')).Hash.ToLowerInvariant()
        source_sidecar = $resourceSidecarSource
        source_manifest = $resourceManifestSource
        built_self_meta = $buildPackageMetaPath
        built_self_meta_sha256 = $packageManifest.built_self_meta_sha256
    }
    foreach ($shaderFile in ($sourceGxpFiles | Sort-Object Name)) {
        $record.shaders[$shaderFile.Name]=Get-FileSha256Hex -LiteralPath $shaderFile.FullName
    }
} else {
    $record.shader_mode='legacy-hash-bundle'
    $record.shader_index_sha256=(Get-FileHash -LiteralPath "$legacyShaders\index.txt").Hash
    foreach ($shaderName in (Get-Content -LiteralPath "$legacyShaders\index.txt")) {
        $shaderName=$shaderName.Trim()
        if (!$shaderName) {continue}
        if ([IO.Path]::GetFileName($shaderName) -ne $shaderName) {throw 'Invalid shader index name'}
        $record.shaders[$shaderName]=(Get-FileHash -LiteralPath (Join-Path "$legacyShaders" $shaderName)).Hash
    }
}
foreach ($name in @('sdk-dso.log','gxm-runtime.log')) {
    if (Test-Path "$data/$name") { Move-Item -LiteralPath "$data/$name" -Destination "$OutDir/previous-$name" }
}
# Rotate prior present dumps too. Their creation time alone cannot distinguish a
# rewritten old file from a new one, so no pre-launch dump may remain in place.
foreach ($pattern in @('display-present-*.rgba', 'b34-present-*.rgba', 'b34-passes.jsonl')) {
    foreach ($file in @(Get-ChildItem -LiteralPath $data -File -Filter $pattern `
            -ErrorAction SilentlyContinue)) {
        Move-Item -LiteralPath $file.FullName -Destination (Join-Path $OutDir ('previous-' + $file.Name))
    }
}

[IO.File]::WriteAllText((Join-Path $OutDir 'process.json'), ($record | ConvertTo-Json -Depth 5), $utf8)
if ($PrepareOnly) { Write-Output "Play prepared: $Config"; return }

# Vita3K GUI otherwise fails on DirectInput haptics in this environment, and
# the SDL audio adapter has no device in the GUI session used for evidence.
$env:SDL_JOYSTICK_DIRECTINPUT = "0"
if ($SilentAudio) { $env:SDL_AUDIODRIVER = "dummy" }
else { Remove-Item Env:SDL_AUDIODRIVER -ErrorAction SilentlyContinue }
# SDL3 reads SDL_AUDIODRIVER. Clear the historical misspelling so a stale
# value can never select a driver silently.
Remove-Item Env:SDL_AUDIO_DRIVER -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Drawing
$captureReferences = @('System.Drawing')
if ($PSVersionTable.PSEdition -eq 'Core') {
    $captureReferences = @([System.Drawing.Bitmap].Assembly.Location,
        [System.Drawing.Point].Assembly.Location,
        (Join-Path $PSHOME 'ref/System.Collections.dll'),
        (Join-Path $PSHOME 'System.Runtime.dll'),
        (Join-Path $PSHOME 'System.Runtime.InteropServices.dll'))
    $captureReferences += @(Get-ChildItem -LiteralPath $PSHOME -Filter 'System.Private.Windows.*.dll' |
        ForEach-Object FullName)
}
if (-not ('VPGuiCap' -as [type])) {
Add-Type -TypeDefinition @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;
public static class VPGuiCap {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll")] public static extern bool ShowWindowAsync(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int w, int h, uint flags);
    [DllImport("user32.dll")] public static extern int GetWindowLong(IntPtr hWnd, int index);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr param);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT rect);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr hWnd, System.Text.StringBuilder text, int max);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetDC(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int ReleaseDC(IntPtr hWnd, IntPtr hdc);
    [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleDC(IntPtr hdc);
    [DllImport("gdi32.dll")] public static extern IntPtr CreateCompatibleBitmap(IntPtr hdc, int w, int h);
    [DllImport("gdi32.dll")] public static extern IntPtr SelectObject(IntPtr hdc, IntPtr obj);
    [DllImport("gdi32.dll")] public static extern bool BitBlt(IntPtr dst, int x, int y, int w, int h,
        IntPtr src, int sx, int sy, uint rop);
    [DllImport("gdi32.dll")] public static extern bool DeleteObject(IntPtr obj);
    [DllImport("gdi32.dll")] public static extern bool DeleteDC(IntPtr hdc);
    public static string CapturePrint(IntPtr h, string path, int timeoutMs) {
        SetProcessDPIAware();
        if (h == IntPtr.Zero || !IsWindow(h)) return "invalid=window-gone";
        string result = "printwindow=timeout";
        var task = System.Threading.Tasks.Task.Run(delegate {
            try {
                RECT r;
                if (!GetWindowRect(h, out r)) { result = "rect=FAILED"; return; }
                int w = r.Right - r.Left, hh = r.Bottom - r.Top;
                if (w <= 0 || hh <= 0) { result = "size=FAILED"; return; }
                using (var bmp = new Bitmap(w, hh))
                using (var g = Graphics.FromImage(bmp)) {
                    IntPtr hdc = g.GetHdc();
                    bool ok;
                    try { ok = PrintWindow(h, hdc, 2); }
                    finally { g.ReleaseHdc(hdc); }
                    if (!ok) { result = "printwindow=FAILED"; return; }
                    bmp.Save(path, ImageFormat.Png);
                    result = Stats(bmp, w, hh);
                }
            } catch (Exception error) {
                result = "printwindow-error=" + error.GetType().Name;
            }
        });
        if (!task.Wait(timeoutMs)) return "printwindow=timeout";
        return result;
    }
    public static string CaptureBlit(IntPtr h, string path) {
        SetProcessDPIAware();
        if (h == IntPtr.Zero || !IsWindow(h)) return "invalid=window-gone";
        RECT r;
        if (!GetWindowRect(h, out r)) return "rect=FAILED";
        int w = r.Right - r.Left, hh = r.Bottom - r.Top;
        if (w <= 0 || hh <= 0) return "size=FAILED";
        // CAPTUREBLT includes layered/DWM-composited content that a plain
        // SRCCOPY screen blit can miss for independent-flip swapchains.
        IntPtr screen = GetDC(IntPtr.Zero);
        IntPtr memory = CreateCompatibleDC(screen);
        IntPtr bitmap = CreateCompatibleBitmap(screen, w, hh);
        IntPtr previous = SelectObject(memory, bitmap);
        try {
            if (!BitBlt(memory, 0, 0, w, hh, screen, r.Left, r.Top, 0x40000000u | 0x00CC0020u))
                return "blit=FAILED";
            using (var bmp = Image.FromHbitmap(bitmap)) {
                bmp.Save(path, ImageFormat.Png);
                return Stats(bmp, w, hh);
            }
        } finally {
            SelectObject(memory, previous);
            DeleteObject(bitmap);
            DeleteDC(memory);
            ReleaseDC(IntPtr.Zero, screen);
        }
    }
    static string Stats(Bitmap bmp, int w, int hh) {
        long nz = 0, total = 0;
        int max = 0;
        var data = bmp.LockBits(new Rectangle(0, 0, w, hh), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try {
            var bytes = new byte[data.Stride * hh];
            Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
            for (int y = 0; y < hh; y += 4) {
                for (int x = 0; x < w; x += 4) {
                    int i = y * data.Stride + x * 4;
                    int m = Math.Max(bytes[i], Math.Max(bytes[i + 1], bytes[i + 2]));
                    if (m > 8) nz++;
                    if (m > max) max = m;
                    total++;
                }
            }
        } finally { bmp.UnlockBits(data); }
        return string.Format("size={0}x{1} samples={2} nonblack={3} max={4}", w, hh, total, nz, max);
    }
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr param);
    // B48: list every top-level window owned by the emulator PID. The game can
    // run in a separate 960x544 presentation window; capturing the launcher
    // main window then truthfully shows no game pixels.
    public static string[] OwnedWindows(int processId) {
        var list = new System.Collections.Generic.List<string>();
        EnumWindows(delegate(IntPtr h, IntPtr param) {
            uint pid;
            GetWindowThreadProcessId(h, out pid);
            if (pid != (uint)processId) return true;
            RECT wr, cr;
            GetWindowRect(h, out wr);
            GetClientRect(h, out cr);
            var title = new System.Text.StringBuilder(512);
            GetWindowText(h, title, title.Capacity);
            list.Add(string.Format("{0}|vis={1}|rect={2},{3},{4},{5}|client={6}x{7}|title={8}",
                h.ToInt64(), IsWindowVisible(h), wr.Left, wr.Top, wr.Right, wr.Bottom,
                cr.Right - cr.Left, cr.Bottom - cr.Top, title.ToString()));
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }
    public static string Capture(IntPtr h, string path) {
        SetProcessDPIAware();
        // Never call the synchronous ShowWindow here. A window whose owner is
        // exiting or blocked can stall that call indefinitely and take the
        // bounded capture path with it. Report the dead window instead.
        if (h == IntPtr.Zero || !IsWindow(h)) return "invalid=window-gone";
        ShowWindowAsync(h, 9);
        RECT r;
        if (!GetWindowRect(h, out r)) return "rect=FAILED";
        int w = r.Right - r.Left, hh = r.Bottom - r.Top;
        if (w <= 0 || hh <= 0) return "size=FAILED";
        using (var bmp = new Bitmap(w, hh))
        using (var g = Graphics.FromImage(bmp)) {
            g.CopyFromScreen(r.Left, r.Top, 0, 0, new Size(w, hh));
            bmp.Save(path, ImageFormat.Png);
            long nz = 0, total = 0;
            int max = 0;
            var data = bmp.LockBits(new Rectangle(0, 0, w, hh), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
            try {
                var bytes = new byte[data.Stride * hh];
                Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
                for (int y = 0; y < hh; y += 4) {
                    for (int x = 0; x < w; x += 4) {
                        int i = y * data.Stride + x * 4;
                        int m = Math.Max(bytes[i], Math.Max(bytes[i + 1], bytes[i + 2]));
                        if (m > 8) nz++;
                        if (m > max) max = m;
                        total++;
                    }
                }
            } finally { bmp.UnlockBits(data); }
            return string.Format("size={0}x{1} samples={2} nonblack={3} max={4}", w, hh, total, nz, max);
        }
    }

    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT point);
    [DllImport("user32.dll")] public static extern IntPtr WindowFromPoint(POINT point);
    [DllImport("user32.dll")] public static extern IntPtr GetAncestor(IntPtr hWnd, uint flags);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr hWnd);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr hWnd, int attribute, out int value, int size);
    [DllImport("kernel32.dll")] static extern void Sleep(uint milliseconds);

    static bool SameRect(RECT a, RECT b) {
        return a.Left == b.Left && a.Top == b.Top && a.Right == b.Right && a.Bottom == b.Bottom;
    }

    static bool SamePoint(POINT a, POINT b) {
        return a.X == b.X && a.Y == b.Y;
    }

    static string Probe(IntPtr h) {
        if (h == IntPtr.Zero || !IsWindow(h)) return "probe_valid=0;reason=window-gone";
        RECT wr = new RECT(), cr = new RECT();
        bool rectOk = GetWindowRect(h, out wr);
        bool clientOk = GetClientRect(h, out cr);
        int cw = clientOk ? cr.Right - cr.Left : 0;
        int ch = clientOk ? cr.Bottom - cr.Top : 0;
        bool visible = IsWindowVisible(h);
        bool iconic = IsIconic(h);
        int cloaked = 0;
        int dwmHr = DwmGetWindowAttribute(h, 14, out cloaked, sizeof(int));
        bool dwmOk = dwmHr == 0;
        if (!dwmOk) { cloaked = 0; }
        IntPtr root = GetAncestor(h, 2);
        int total = 0, hits = 0;
        string points = "";
        if (clientOk && cw > 2 && ch > 2) {
            int[] xs = { 1, cw - 2, 1, cw - 2, cw / 2 };
            int[] ys = { 1, 1, ch - 2, ch - 2, ch / 2 };
            for (int i = 0; i < 5; ++i) {
                POINT p; p.X = xs[i]; p.Y = ys[i];
                ClientToScreen(h, ref p);
                IntPtr hit = WindowFromPoint(p);
                IntPtr hitRoot = hit == IntPtr.Zero ? IntPtr.Zero : GetAncestor(hit, 2);
                bool ok = root != IntPtr.Zero && hitRoot == root;
                if (ok) ++hits;
                ++total;
                if (i > 0) points += ",";
                points += (ok ? "1" : "0");
            }
        }
        POINT origin = new POINT();
        bool originOk = clientOk && ClientToScreen(h, ref origin);
        bool foreground = GetForegroundWindow() == h;
        uint dpi = 0;
        try { dpi = GetDpiForWindow(h); } catch { }
        var title = new System.Text.StringBuilder(512);
        GetWindowText(h, title, title.Capacity);
        string titleB64 = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(title.ToString()));
        uint pid = 0;
        GetWindowThreadProcessId(h, out pid);
        return string.Format(
            "probe_valid=1;hwnd={0};pid={1};visible={2};iconic={3};cloaked={4};dwm_ok={5};dwm_hr={6};" +
            "foreground={7};dpi={8};rect_ok={9};rect={10},{11},{12},{13};client_ok={14};client={15}x{16};" +
            "origin_ok={17};origin={18},{19};hits={20};total={21};points={22};title_b64={23}",
            h.ToInt64(), pid, visible ? 1 : 0, iconic ? 1 : 0,
            (dwmOk && cloaked != 0) ? 1 : 0, dwmOk ? 1 : 0, dwmHr,
            foreground ? 1 : 0, dpi,
            rectOk ? 1 : 0, wr.Left, wr.Top, wr.Right, wr.Bottom,
            clientOk ? 1 : 0, cw, ch,
            originOk ? 1 : 0, origin.X, origin.Y,
            hits, total, points, titleB64);
    }

    static string StatsRegion(Bitmap bmp, int x0, int y0, int w, int h) {
        if (w <= 0 || h <= 0) return "samples=0;nonblack=0;max=0;uniform=1;allwhite=0;allblack=1";
        // Inset before sampling: Windows 11 rounds top-level window corners, so
        // the desktop can show through the corners of the rectangular client
        // rect. Sampling that chrome would make an otherwise uniform white
        // placeholder look non-uniform. The full client is still in the PNG;
        // only the statistics exclude a 12px border.
        int inset = 12;
        if (w > 2 * inset + 8 && h > 2 * inset + 8) {
            x0 += inset; y0 += inset; w -= 2 * inset; h -= 2 * inset;
        }
        x0 = Math.Max(0, x0); y0 = Math.Max(0, y0);
        w = Math.Min(w, bmp.Width - x0); h = Math.Min(h, bmp.Height - y0);
        if (w <= 0 || h <= 0) return "samples=0;nonblack=0;max=0;uniform=1;allwhite=0;allblack=1";
        long samples = 0, nonblack = 0;
        int max = 0, min = 255;
        bool uniform = true, allwhite = true, allblack = true;
        int firstB = -1, firstG = -1, firstR = -1;
        var data = bmp.LockBits(new Rectangle(x0, y0, w, h), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try {
            var bytes = new byte[data.Stride * h];
            Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
            for (int y = 0; y < h; y += 4) {
                for (int x = 0; x < w; x += 4) {
                    int i = y * data.Stride + x * 4;
                    int b = bytes[i], g = bytes[i + 1], r = bytes[i + 2];
                    int m = Math.Max(b, Math.Max(g, r));
                    if (m > 8) nonblack++;
                    if (m > max) max = m;
                    if (m < min) min = m;
                    if (firstB < 0) { firstB = b; firstG = g; firstR = r; }
                    else if (b != firstB || g != firstG || r != firstR) uniform = false;
                    if (b < 250 || g < 250 || r < 250) allwhite = false;
                    if (b > 8 || g > 8 || r > 8) allblack = false;
                    samples++;
                }
            }
        } finally { bmp.UnlockBits(data); }
        return string.Format(
            "samples={0};nonblack={1};max={2};min={3};uniform={4};allwhite={5};allblack={6}",
            samples, nonblack, max, min, uniform ? 1 : 0, allwhite ? 1 : 0, allblack ? 1 : 0);
    }

    // Bounded verified capture. The window must stay at a stable physical
    // geometry AND client origin and every sampled client point must resolve to
    // the same top-level window before and after the screen copy. Occlusion is
    // deliberately not part of the stability wait: a stable-but-occluded window
    // is captured and then rejected by the pure verdict with reason=occluded.
    // The client area is sampled separately from the title bar so a white/black
    // Qt filler is not mistaken for the client's contents. The five hit points
    // are a bounded probe, not full-pixel occlusion proof.
    public static string CaptureVerified(IntPtr h, string path, int timeoutMs) {
        if (h == IntPtr.Zero || !IsWindow(h)) return "usable=0;reason=window-gone";
        int limit = Math.Max(500, Math.Min(timeoutMs, 8000));
        int elapsed = 0;
        bool stable = false;
        RECT lastWr = new RECT(), lastCr = new RECT();
        POINT lastOrigin = new POINT();
        while (elapsed <= limit) {
            RECT wr, cr;
            POINT origin = new POINT();
            if (!GetWindowRect(h, out wr) || !GetClientRect(h, out cr) ||
                !ClientToScreen(h, ref origin)) {
                return "usable=0;reason=geometry-failed;" + Probe(h);
            }
            string probe = Probe(h);
            bool usable = probe.Contains("probe_valid=1") && probe.Contains("visible=1") &&
                probe.Contains("iconic=0") && probe.Contains("dwm_ok=1") &&
                probe.Contains("cloaked=0") && probe.Contains("rect_ok=1") &&
                probe.Contains("client_ok=1");
            if (usable && elapsed > 0 && SameRect(wr, lastWr) && SameRect(cr, lastCr) &&
                SamePoint(origin, lastOrigin)) {
                stable = true;
                break;
            }
            lastWr = wr; lastCr = cr; lastOrigin = origin;
            Sleep(250);
            elapsed += 250;
        }
        if (!stable) return "usable=0;reason=geometry-unstable;" + Probe(h);
        RECT wr2, cr2;
        POINT origin2 = new POINT();
        if (!GetWindowRect(h, out wr2) || !GetClientRect(h, out cr2) ||
            !ClientToScreen(h, ref origin2)) {
            return "usable=0;reason=geometry-failed;" + Probe(h);
        }
        int w = wr2.Right - wr2.Left, hh = wr2.Bottom - wr2.Top;
        int cw = cr2.Right - cr2.Left, ch = cr2.Bottom - cr2.Top;
        if (w <= 0 || hh <= 0 || cw <= 0 || ch <= 0) return "usable=0;reason=size-failed";
        int cx = origin2.X - wr2.Left, cy = origin2.Y - wr2.Top;
        string stats;
        try {
            using (var bmp = new Bitmap(w, hh))
            using (var g = Graphics.FromImage(bmp)) {
                g.CopyFromScreen(wr2.Left, wr2.Top, 0, 0, new Size(w, hh));
                bmp.Save(path, ImageFormat.Png);
                stats = StatsRegion(bmp, cx, cy, cw, ch);
            }
        } catch (Exception error) {
            return "usable=0;reason=capture-error-" + error.GetType().Name;
        }
        string post = Probe(h);
        RECT postWr, postCr;
        POINT postOrigin = new POINT();
        bool postGeometry = GetWindowRect(h, out postWr) && GetClientRect(h, out postCr) &&
            ClientToScreen(h, ref postOrigin) && SameRect(wr2, postWr) &&
            SameRect(cr2, postCr) && SamePoint(origin2, postOrigin);
        return string.Format(
            "usable=1;reason=usable;stable=1;post_stable={0};rect={1},{2},{3},{4};" +
            "client={5}x{6};client_origin={7},{8};{9};{10}",
            postGeometry ? 1 : 0, wr2.Left, wr2.Top, wr2.Right, wr2.Bottom,
            cw, ch, cx, cy, stats, post);
    }
}
"@ -ReferencedAssemblies $captureReferences
}

$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
. (Join-Path $PSScriptRoot 'detached_process.ps1')
$launchArguments = @(
    "--load-config", "--config-location", $Config, "--keep-config",
    "--backend-renderer", $requestedRenderer, "--installed-path", "MHGUVP018"
)
# Preserve the previous log so an old renderer initialization cannot qualify a
# new launch that never reaches renderer creation.
$engineLog = Join-Path (Split-Path $Exe) 'vita3k.log'
if (Test-Path -LiteralPath $engineLog) {
    Copy-Item -LiteralPath $engineLog -Destination (Join-Path $OutDir 'previous-vita3k.log')
    Remove-Item -LiteralPath $engineLog -Force
}
$backendLaunchUtc = [datetime]::UtcNow
# The runtime log is append-only across launches. Remember where this launch
# starts so present evidence from an earlier process cannot qualify a capture.
$gxmLogPath = Join-Path $data 'gxm-runtime.log'
$gxmLogPresentAtBaseline = Test-Path -LiteralPath $gxmLogPath
$gxmLogBaseline = if ($gxmLogPresentAtBaseline) {
    (Get-Item -LiteralPath $gxmLogPath).Length
} else { [long]0 }
$record.gxm_log_path = $gxmLogPath
$record.gxm_log_present_at_baseline = $gxmLogPresentAtBaseline
$record.gxm_log_baseline_length = $gxmLogBaseline
if ($NoDetach) {
    # Bounded foreground runs do not need to outlive this runner. Some CI/sandbox
    # jobs prohibit child breakaway; a plain child stays in that job and is
    # stopped by the runner's existing bounded cleanup.
    $proc = Start-Process -FilePath $Exe -ArgumentList $launchArguments -PassThru
    $record.launch_creation_flags = '0x00000000'
    $record.launch_child_in_any_job = $true
} else {
    $proc = Start-VPDetachedProcess -FilePath $Exe -Arguments $launchArguments
    $record.launch_creation_flags = ('0x{0:X8}' -f [VPDetachedProcess]::LastCreationFlags)
    $record.launch_child_in_any_job = [VPDetachedProcess]::LastChildInJob
}
$record.pid = $proc.Id
$record.started_utc = [datetime]::UtcNow.ToString('o')
[IO.File]::WriteAllText((Join-Path $OutDir 'process.json'), ($record | ConvertTo-Json -Depth 5), $utf8)

# Attach a separate observer immediately: it retains the process handle and
# records the real exit code/elapsed time even after this runner exits (the
# KeepRunning path returns as soon as captures finish). The observer only reads
# the process and writes its own JSON; it never stops or restarts the title.
$record.observer_script = 'tools/runtime/observe_process_lifecycle.ps1'
$record.observer_output = Join-Path $OutDir 'process-lifecycle.json'
$record.observer_ready = Join-Path $OutDir 'process-lifecycle.ready.json'
$record.observer_ready_observed = $false
$record.observer_error = $null
$record.observer_pid = $null
if (Test-Path -LiteralPath (Join-Path $OutDir 'process-lifecycle.json')) {
    Remove-Item -LiteralPath (Join-Path $OutDir 'process-lifecycle.json') -Force
}
if (Test-Path -LiteralPath (Join-Path $OutDir 'process-lifecycle.ready.json')) {
    Remove-Item -LiteralPath (Join-Path $OutDir 'process-lifecycle.ready.json') -Force
}
$observerScriptPath = Join-Path $PSScriptRoot 'observe_process_lifecycle.ps1'
$hostExe = try { (Get-Process -Id $PID).Path } catch { $null }
if (-not $hostExe) {
    $candidate = Join-Path $PSHOME 'pwsh.exe'
    if (-not (Test-Path -LiteralPath $candidate)) {
        $candidate = Join-Path $PSHOME 'powershell.exe'
    }
    $hostExe = $candidate
}
try {
    $procStartUtc = $proc.StartTime.ToUniversalTime().ToString('o')
} catch {
    $procStartUtc = $record.started_utc
}
$observerProcessName = try { $proc.ProcessName } catch { 'Vita3K' }
$record.observer_expected_start_utc = $procStartUtc
# Pass the log path array through a UTF-8 JSON file. A flat Start-Process
# argument list does not preserve the JSON quotes/brackets of -LogPathsJson,
# which made the observer fail before writing its ready receipt.
$observerLogPathsFile = Join-Path $OutDir 'observer-log-paths.json'
[IO.File]::WriteAllText($observerLogPathsFile,
    (ConvertTo-Json -InputObject @((Join-Path $data 'sdk-dso.log'), (Join-Path $data 'gxm-runtime.log')) -Compress), $utf8)
$record.observer_log_paths_file = $observerLogPathsFile
if ((Test-Path -LiteralPath $observerScriptPath) -and $hostExe -and (Test-Path -LiteralPath $hostExe)) {
    $observerArguments = @(
        '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
        '-File', $observerScriptPath,
        '-AttachPid', "$($proc.Id)",
        '-ExpectedStartTimeUtc', $procStartUtc,
        '-OutputPath', $record.observer_output,
        '-ReadyPath', $record.observer_ready,
        '-LogPathsFile', ('"' + $observerLogPathsFile + '"'),
        '-ProcessName', $observerProcessName
    )
    try {
        if ($NoDetach) {
            $observerStdout = Join-Path $OutDir 'observer-stdout.txt'
            $observerStderr = Join-Path $OutDir 'observer-stderr.txt'
            $observer = Start-Process -FilePath $hostExe -ArgumentList $observerArguments `
                -WindowStyle Hidden -PassThru `
                -RedirectStandardOutput $observerStdout -RedirectStandardError $observerStderr
        } else {
            $observer = Start-VPDetachedProcess -FilePath $hostExe -Arguments $observerArguments -Hidden
        }
        $record.observer_pid = $observer.Id
        $record.observer_started_utc = [datetime]::UtcNow.ToString('o')
        $readyDeadline = (Get-Date).AddSeconds(5)
        while ((Get-Date) -lt $readyDeadline -and
               -not (Test-Path -LiteralPath $record.observer_ready)) {
            Start-Sleep -Milliseconds 100
            $proc.Refresh()
            if ($proc.HasExited) { break }
        }
        if (Test-Path -LiteralPath $record.observer_ready) {
            $record.observer_ready_observed = $true
            try {
                $readyJson = [IO.File]::ReadAllText(
                    $record.observer_ready, [Text.UTF8Encoding]::new($false)) | ConvertFrom-Json
                $record.observer_target_pid = $readyJson.target_pid
                $record.observer_handle_opened = $readyJson.handle_opened
                $record.observer_job = $readyJson.job
            } catch {
                $record.observer_error = "ready receipt unreadable: $($_.Exception.Message)"
            }
        } else {
            $record.observer_error = 'observer ready receipt not observed within 5s'
        }
    } catch {
        $record.observer_error = $_.Exception.Message
    }
} else {
    $record.observer_error = 'observer script or host executable missing'
}
[IO.File]::WriteAllText((Join-Path $OutDir 'process.json'), ($record | ConvertTo-Json -Depth 8), $utf8)

$window = [IntPtr]::Zero
try {
$windowDeadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $windowDeadline) {
    Start-Sleep -Milliseconds 250
    $proc.Refresh()
    if ($proc.HasExited) { break }
    if ($proc.MainWindowHandle -ne 0) { $window = $proc.MainWindowHandle; break }
}
if ($window -eq [IntPtr]::Zero) {
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
    throw "Vita3K window did not appear"
}

$started = Get-Date
$record.capture_attempts = 0
$record.capture_usable_count = 0
$record.capture_high_coverage_count = 0
$record.capture_rejected = @()
foreach ($seconds in $captureSchedule) {
    $target = $started.AddSeconds([double]$seconds)
    # Sleep in bounded chunks: a stale/odd target must not overflow the
    # Int32 millisecond limit of Start-Sleep.
    while ($true) {
        $proc.Refresh()
        if ($proc.HasExited) { break }
        $remaining = ($target - (Get-Date)).TotalSeconds
        if ($remaining -le 0) { break }
        $ms = [int][Math]::Ceiling([Math]::Min($remaining * 1000.0, 1000.0))
        if ($ms -le 0) { break }
        Start-Sleep -Milliseconds $ms
    }
    $proc.Refresh()
    if ($proc.HasExited) {
        # Preserve the failure while allowing finally to collect evidence.
        $record.capture_failure = 'process-exited'
        "capture t=${seconds}s pid=$($proc.Id) skipped=process-exited" |
            Tee-Object -FilePath (Join-Path $OutDir "capture.txt") -Append
        break
    }
    # SDL can replace the launcher window when the title starts. Enumerate the
    # owned windows because the game may present in a separate 960x544 window;
    # record the complete candidate list next to the capture.
    if ($proc.MainWindowHandle -ne 0) { $window=$proc.MainWindowHandle }
    $ownedWindows = @()
    try { $ownedWindows = @([VPGuiCap]::OwnedWindows([int]$proc.Id)) } catch { $ownedWindows = @() }
    $choice = Select-VPGameCaptureWindow -Windows $ownedWindows -Fallback $window
    $window = $choice.Handle
    ("capture-windows t=${seconds}s pid=$($proc.Id) chosen=$($choice.Info)") |
        Tee-Object -FilePath (Join-Path $OutDir 'capture-windows.txt') -Append
    foreach ($ownedLine in $ownedWindows) {
        ("owned " + $ownedLine) |
            Tee-Object -FilePath (Join-Path $OutDir 'capture-windows.txt') -Append
    }
    if (-not [VPGuiCap]::IsWindow($window)) {
        # A destroyed window cannot be captured, and activating it can block
        # on a process that is shutting down. Never fabricate a screenshot.
        $record.capture_failure = 'window-gone'
        "capture t=${seconds}s pid=$($proc.Id) skipped=window-gone handle=$window" |
            Tee-Object -FilePath (Join-Path $OutDir "capture.txt") -Append
        break
    }
    # A capture is USABLE EVIDENCE only when the guest presented in this launch
    # and the owned window is visible, unoccluded at the sampled points,
    # geometrically/client-origin stable, and non-uniform. That is not a
    # semantic game-content verdict: the preserved PNG must still be inspected.
    # The readiness wait is bounded by the run's absolute deadline and by the
    # owned process's liveness. Foreground activation is never requested
    # synchronously; the topmost toggle is best effort.
    $wasTopmost = ([VPGuiCap]::GetWindowLong($window, -20) -band 8) -ne 0
    $wasMinimized = $false
    try { $wasMinimized = [VPGuiCap]::IsIconic($window) } catch { }
    $captureStartedUtc = [datetime]::UtcNow
    $readyDeadline = $captureStartedUtc.AddSeconds([Math]::Max(0, $CaptureReadyTimeoutSeconds))
    if ($ExitAfterSeconds -gt 0) {
        $budgetDeadline = $started.AddSeconds([Math]::Max(0, $ExitAfterSeconds - 15))
        if ($budgetDeadline -lt $readyDeadline) { $readyDeadline = $budgetDeadline }
    }
    $attemptShot = Join-Path $OutDir ("window-{0:D3}s-attempt.png" -f $seconds)
    $finalShot = Join-Path $OutDir ("window-{0:D3}s.png" -f $seconds)
    $captureAttempts = 0
    $captureStats = 'usable=0;reason=not-attempted'
    $captureFields = @{}
    $captureVerdict = [pscustomobject]@{ Usable = $false; Reason = 'not-attempted'; ContentClass = 'empty' }
    $presentEvidence = [pscustomobject]@{
        PresentSinceLaunch = $false; Scope = 'since-launch'; Source = 'none'
        Count = 0; Detail = ''; LogCreatedSinceLaunch = $false; ScannedFrom = 0
    }
    $attemptDetails = New-Object System.Collections.Generic.List[object]
    $foreground = $false
    $captureAborted = $false
    $finalWritten = $false
    $finalSha = ''
    try {
        [VPGuiCap]::ShowWindowAsync($window, 9) | Out-Null
        [VPGuiCap]::SetWindowPos($window, [IntPtr](-1), 0, 0, 0, 0, 0x4043) | Out-Null
        Start-Sleep -Milliseconds 500
        while ($true) {
            $proc.Refresh()
            if ($proc.HasExited) { $captureAborted = $true; break }
            $captureAttempts++
            $attemptUtc = [datetime]::UtcNow
            $presentEvidence = Get-VPGuestPresentEvidence -LogPath $gxmLogPath `
                -BaselineLength $gxmLogBaseline -SinceUtc $backendLaunchUtc -DataDir $data
            $remainingMs = [int][Math]::Max(500, [Math]::Min(8000,
                ($readyDeadline - (Get-Date)).TotalMilliseconds))
            $captureStats = [VPGuiCap]::CaptureVerified($window, $attemptShot, $remainingMs)
            $captureFields = ConvertFrom-VPCaptureReceipt -Text $captureStats
            $foreground = [VPGuiCap]::GetForegroundWindow() -eq $window
            $ownerMatch = ($captureFields['pid'] -eq [string]$proc.Id) -and
                [VPGuiCap]::IsWindow($window)
            $captureVerdict = Get-WindowCaptureVerdict `
                -ProbeValid ($captureFields['probe_valid'] -eq '1') `
                -DwmOk ($captureFields['dwm_ok'] -eq '1') `
                -GeometryOk ($captureFields['rect_ok'] -eq '1') `
                -ClientOk ($captureFields['client_ok'] -eq '1') `
                -OwnerMatch $ownerMatch `
                -Visible ($captureFields['visible'] -eq '1') `
                -Iconic ($captureFields['iconic'] -eq '1') `
                -Cloaked ($captureFields['cloaked'] -eq '1') `
                -GeometryStable (($captureFields['stable'] -eq '1') -and
                                 ($captureFields['post_stable'] -eq '1')) `
                -ClientOriginStable (($captureFields['origin_ok'] -eq '1') -and
                                     ($captureFields['post_stable'] -eq '1')) `
                -HitPoints ([int]$captureFields['hits']) `
                -TotalPoints ([int]$captureFields['total']) `
                -GuestPresentSinceLaunch ([bool]$presentEvidence.PresentSinceLaunch) `
                -Samples ([long]$captureFields['samples']) `
                -NonBlack ([long]$captureFields['nonblack']) `
                -Max ([int]$captureFields['max']) `
                -Uniform ($captureFields['uniform'] -eq '1') `
                -AllWhite ($captureFields['allwhite'] -eq '1') `
                -AllBlack ($captureFields['allblack'] -eq '1')
            if ($attemptDetails.Count -lt 200) {
                $attemptDetails.Add([pscustomobject]@{
                    attempt = $captureAttempts
                    utc = $attemptUtc.ToString('o')
                    usable = [bool]$captureVerdict.Usable
                    reason = $captureVerdict.Reason
                    content_class = $captureVerdict.ContentClass
                    stats = $captureStats
                })
            }
            if ($captureVerdict.Usable) {
                Copy-Item -LiteralPath $attemptShot -Destination $finalShot -Force
                $finalWritten = $true
                $finalSha = (Get-FileHash -LiteralPath $finalShot -Algorithm SHA256).Hash
                break
            }
            if ($captureVerdict.Reason -eq 'occluded' -or $captureVerdict.Reason -eq 'minimized') {
                # Re-issue the non-blocking raise on every retry: a one-shot
                # topmost request can lose to a window that raises itself
                # while the guest is still starting. Never use the synchronous
                # SetForegroundWindow here.
                if ($captureVerdict.Reason -eq 'minimized') {
                    [VPGuiCap]::ShowWindowAsync($window, 9) | Out-Null
                }
                [VPGuiCap]::SetWindowPos($window, [IntPtr](-1), 0, 0, 0, 0, 0x4043) | Out-Null
            }
            $proc.Refresh()
            if ($proc.HasExited) { $captureAborted = $true; break }
            if ((Get-Date) -ge $readyDeadline) { break }
            Start-Sleep -Milliseconds 500
        }
    } finally {
        if (-not $wasTopmost) {
            [VPGuiCap]::SetWindowPos($window, [IntPtr](-2), 0, 0, 0, 0, 0x4013) | Out-Null
        }
    }
    $captureCompletedUtc = [datetime]::UtcNow
    $captureElapsed = ($captureCompletedUtc - $captureStartedUtc).TotalSeconds
    $finalFileValue = if ($finalWritten) { [string]$finalShot } else { '' }
    # Materialize the generic list before the [ordered] literal: wrapping a
    # List[object] with @() inline makes the OrderedDictionary cast fail with
    # "Argument types do not match" in both Windows PowerShell and pwsh.
    $attemptArray = $attemptDetails.ToArray()
    $record.capture_attempts += $captureAttempts
    if ($captureVerdict.Usable) { $record.capture_usable_count += 1 }
    if ($captureVerdict.Usable -and $captureVerdict.ContentClass -eq 'content') {
        $record.capture_high_coverage_count += 1
    }
    $captureRecord = [ordered]@{
        nominal_seconds = $seconds
        attempts = $captureAttempts
        usable = [bool]$captureVerdict.Usable
        reason = $captureVerdict.Reason
        content_class = $captureVerdict.ContentClass
        requires_visual_review = $true
        capture_started_utc = $captureStartedUtc.ToString('o')
        capture_completed_utc = $captureCompletedUtc.ToString('o')
        elapsed_seconds = [Math]::Round($captureElapsed, 3)
        final_written = $finalWritten
        final_file = $finalFileValue
        final_sha256 = $finalSha
        attempt_file = $attemptShot
        present_scope = $presentEvidence.Scope
        present_since_launch = [bool]$presentEvidence.PresentSinceLaunch
        present_source = $presentEvidence.Source
        present_count = $presentEvidence.Count
        present_detail = $presentEvidence.Detail
        present_log_created_since_launch = [bool]$presentEvidence.LogCreatedSinceLaunch
        present_scanned_from = $presentEvidence.ScannedFrom
        foreground = $foreground
        was_minimized = $wasMinimized
        hwnd = $window.ToInt64()
        owner_pid_expected = $proc.Id
        owner_pid_observed = $captureFields['pid']
        owner_match = ($captureFields['pid'] -eq [string]$proc.Id)
        probe_valid = $captureFields['probe_valid']
        dwm_ok = $captureFields['dwm_ok']
        rect_ok = $captureFields['rect_ok']
        client_ok = $captureFields['client_ok']
        title_b64 = $captureFields['title_b64']
        rect = $captureFields['rect']
        client = $captureFields['client']
        origin = $captureFields['origin']
        dpi = $captureFields['dpi']
        hits = $captureFields['hits']
        total = $captureFields['total']
        samples = $captureFields['samples']
        nonblack = $captureFields['nonblack']
        max = $captureFields['max']
        uniform = $captureFields['uniform']
        allwhite = $captureFields['allwhite']
        allblack = $captureFields['allblack']
        stats = $captureStats
        attempts_detail = $attemptArray
    }
    $captureRecord | ConvertTo-Json -Depth 8 |
        Set-Content -LiteralPath (Join-Path $OutDir ("window-{0:D3}s.capture.json" -f $seconds)) -Encoding utf8
    if (-not $captureVerdict.Usable) {
        $record.capture_rejected = @($record.capture_rejected) + @([pscustomobject]@{
            t_seconds = $seconds; attempts = $captureAttempts; elapsed_seconds = [Math]::Round($captureElapsed, 3)
            reason = $captureVerdict.Reason; content_class = $captureVerdict.ContentClass
            present_source = $presentEvidence.Source; present_count = $presentEvidence.Count
            attempt_file = $attemptShot; stats = $captureStats
        })
    }
    "capture t=${seconds}s pid=$($proc.Id) title=$($proc.MainWindowTitle) foreground=$foreground was_minimized=$wasMinimized " +
        "attempts=$captureAttempts elapsed=$([Math]::Round($captureElapsed,1))s usable=$([int]$captureVerdict.Usable) " +
        "verdict=$($captureVerdict.Reason) content=$($captureVerdict.ContentClass) final=$finalWritten " +
        "present=$($presentEvidence.Source):$($presentEvidence.Count):$($presentEvidence.Scope) $captureStats" |
        Tee-Object -FilePath (Join-Path $OutDir "capture.txt") -Append
    if ($captureAborted) {
        $record.capture_failure = 'process-exited'
        "capture t=${seconds}s pid=$($proc.Id) skipped=process-exited-during-readiness" |
            Tee-Object -FilePath (Join-Path $OutDir "capture.txt") -Append
        break
    }
    if ($CaptureCompatibilityShots) {
        $printShot = Join-Path $OutDir ("window-{0:D3}s-print.png" -f $seconds)
        $printStats = [VPGuiCap]::CapturePrint($window, $printShot, 8000)
        $blitShot = Join-Path $OutDir ("window-{0:D3}s-blit.png" -f $seconds)
        $blitStats = [VPGuiCap]::CaptureBlit($window, $blitShot)
        "capture-print t=${seconds}s pid=$($proc.Id) file=$printShot $printStats" |
            Tee-Object -FilePath (Join-Path $OutDir "capture.txt") -Append
        "capture-blit t=${seconds}s pid=$($proc.Id) file=$blitShot $blitStats" |
            Tee-Object -FilePath (Join-Path $OutDir "capture.txt") -Append
    }
}
if ($captureSchedule.Count -gt 0 -and $record.capture_usable_count -le 0 -and
    -not $record.Contains('capture_failure')) {
    $record.capture_failure = 'no-usable-capture'
}

if (-not $KeepRunning) {
    $elapsed = ((Get-Date) - $started).TotalSeconds
    if ($ExitAfterSeconds -gt $elapsed) {
        $remaining = [int]($ExitAfterSeconds - $elapsed)
        # If the guest already requested exit, the window is gone. Do not spend
        # the whole budget waiting on a stuck emulator shutdown; allow a short
        # bounded grace, then close/stop the pid this runner owns.
        $grace = if ([VPGuiCap]::IsWindow($window)) { $remaining } else { [Math]::Min(5, $remaining) }
        if ($grace -gt 0) { Start-Sleep -Seconds $grace }
    }
    $proc.Refresh()
    $record.exited_before_cleanup = $proc.HasExited
    $record.graceful_exit = $false
    if (-not $proc.HasExited -and $GracefulExitSeconds -gt 0) {
        # B48: the emulator's own close path (frontend shutdown + real process
        # exit code) instead of Stop-Process. The helper refuses windows that
        # do not belong to the owned PID and reports a bounded timeout.
        $proc.Refresh()
        $closeWindows = @()
        try { $closeWindows = @([VPGuiCap]::OwnedWindows([int]$proc.Id)) } catch { $closeWindows = @() }
        $mainChoice = Select-VPMainWindow -Windows $closeWindows -Fallback $window
        $record.graceful_close_target = $mainChoice.Info
        $record.graceful_close = Close-OwnedWindowGracefully -Window $mainChoice.Handle `
            -Process $proc -TimeoutSeconds $GracefulExitSeconds
        $record.graceful_exit = [bool]$record.graceful_close.exit_observed
        if (-not $record.graceful_exit -and $CaptureShutdownDump -and -not $proc.HasExited) {
            $record.shutdown_dump = Write-OwnedProcessMinidump -Process $proc `
                -OutputPath (Join-Path $OutDir 'shutdown-hang.dmp')
        }
        $proc.Refresh()
    }
    if (-not $proc.HasExited) {
        $record.launcher_stopped = $true
        $record.graceful_exit = $false
        Stop-Process -Id $proc.Id -Force
        $proc.WaitForExit(15000) | Out-Null
    }
    $record.graceful_exit_required = [bool]$RequireGracefulExit
}
"gui run pid=$($proc.Id) config=$Config" |
    Tee-Object -FilePath (Join-Path $OutDir "capture.txt") -Append
} finally {
$proc.Refresh()
if (-not $record.Contains('exited_before_cleanup')) { $record.exited_before_cleanup = $proc.HasExited }
if ($proc.HasExited) { $record.exit_code = $proc.ExitCode }
$record.observed_until_utc = [datetime]::UtcNow.ToString('o')
if (-not $KeepRunning -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
foreach ($name in @('sdk-dso.log','gxm-runtime.log')) {
    if (Test-Path "$data/$name") { Copy-Item -LiteralPath "$data/$name" -Destination "$OutDir/$name" }
}
$engineLog = Join-Path (Split-Path $Exe) 'vita3k.log'
if (Test-Path $engineLog) { Copy-Item -LiteralPath $engineLog -Destination "$OutDir/vita3k.log" }
# Restore the bounded-batch marker state captured before launch. The emulator is
# already stopped, so no process can observe the transition.
if ($LiveInput -and $proc.HasExited) {
    foreach ($name in @('input-live.enable','input-live.txt','input-live.tmp')) {
        $livePath = Join-Path $data $name
        if (Test-Path -LiteralPath $livePath) { Remove-Item -LiteralPath $livePath -Force }
    }
}
if ($PlayPerf -and $proc.HasExited) {
    Remove-Item -LiteralPath (Join-Path $data 'play-perf.enable') -Force -ErrorAction SilentlyContinue
}
if ($LiveCapture -and $proc.HasExited) {
    foreach ($name in @('capture-live.enable','capture-request.txt','capture-request.tmp')) {
        Remove-Item -LiteralPath (Join-Path $data $name) -Force -ErrorAction SilentlyContinue
    }
}
if ($null -ne $boundedBatchSaved) {
    foreach ($name in $boundedBatchNames) {
        $markerPath = Join-Path $data $name
        if (Test-Path -LiteralPath $markerPath) {
            Remove-Item -LiteralPath $markerPath -Force
        }
        if ($boundedBatchSaved.ContainsKey($name)) {
            [IO.File]::WriteAllBytes($markerPath, [byte[]]$boundedBatchSaved[$name])
        }
    }
}
if ($CaptureModels) {
    foreach ($file in (Get-ChildItem -LiteralPath $data -File -Filter 'creation-*')) {
        if ($file.Name -eq 'creation-draws.jsonl' -or
            ($file.Name -match '^creation-(\d+)-' -and [int]$Matches[1] -lt 12)) {
            Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $OutDir $file.Name)
        }
    }
}
if ($CaptureAudio) {
    foreach ($file in (Get-ChildItem -LiteralPath $data -File -Filter 'audio-*.bin')) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $OutDir $file.Name)
    }
}
# The observer may have already produced its final record (bounded runs). Its
# file is authoritative for the real exit code; do not infer a natural exit
# from PID absence here.
$observerFinalPath = Join-Path $OutDir 'process-lifecycle.json'
if ($record.observer_ready_observed) {
    # A graceful close exits without the runner killing the process, so give
    # the observer a bounded moment to observe the retained handle and write
    # its final record before this runner reads it.
    $observerWaitDeadline = (Get-Date).AddSeconds(10)
    while (-not (Test-Path -LiteralPath $observerFinalPath) -and
           (Get-Date) -lt $observerWaitDeadline) {
        Start-Sleep -Milliseconds 200
    }
}
if (Test-Path -LiteralPath $observerFinalPath) {
    try {
        # Explicit UTF-8: Windows PowerShell would otherwise decode the
        # observer's no-BOM file as ANSI, corrupting localized event-log text.
        $observerFinal = [IO.File]::ReadAllText(
            $observerFinalPath, [Text.UTF8Encoding]::new($false)) | ConvertFrom-Json
        $record.observer_classification = $observerFinal.classification
        $record.observer_exit_code = $observerFinal.exit_code
        $record.observer_elapsed_seconds = $observerFinal.elapsed_seconds
        $record.observer_caller_stop = $observerFinal.caller_stop
        $record.observer_crash_suspected = $observerFinal.crash_suspected
        $record.observer_pid_reused = $observerFinal.pid_reused
    } catch {
        $record.observer_final_error = $_.Exception.Message
    }
}
# B48: the effective audio adapter is decided inside the emulator after the
# config merge, so record it from the fresh engine log (not from the request).
$record.audio_backend_effective = $null
$record.audio_backend_initialized = $false
$record.audio_backend_failed = $false
$runEngineLog = Join-Path $OutDir 'vita3k.log'
if (Test-Path -LiteralPath $runEngineLog) {
    $logText = (Get-Content -LiteralPath $runEngineLog -TotalCount 20000) -join "`n"
    $audioOk = [regex]::Match($logText, 'Audio backend (\w+) initialized')
    $audioBad = [regex]::Match($logText, 'Audio backend (\w+) failed to initialize')
    if ($audioOk.Success) {
        $record.audio_backend_effective = $audioOk.Groups[1].Value
        $record.audio_backend_initialized = $true
    } elseif ($audioBad.Success) {
        $record.audio_backend_effective = $audioBad.Groups[1].Value
        $record.audio_backend_failed = $true
    }
}
[IO.File]::WriteAllText((Join-Path $OutDir 'process.json'), ($record | ConvertTo-Json -Depth 8), $utf8)
}

Assert-RendererBackendLog (Join-Path $OutDir 'vita3k.log') (Join-Path $OutDir 'backend-verification.json') $backendLaunchUtc $requestedRenderer
if ($RequireAudioAdapter) {
    if (-not $record.audio_backend_initialized -or
        $record.audio_backend_effective -ne $RequireAudioAdapter) {
        throw ("Audio adapter requirement not met: required=$RequireAudioAdapter " +
            "effective=$($record.audio_backend_effective) initialized=$($record.audio_backend_initialized); " +
            "evidence saved in $OutDir")
    }
}
$gracefulObserved = [bool]$record.graceful_exit -or
    ([bool]$record.exited_before_cleanup -and $null -ne $record.exit_code -and $record.exit_code -eq 0)
if ($RequireGracefulExit -and -not $gracefulObserved) {
    throw "Graceful exit requirement not met (fallback force stop or abnormal early exit); evidence saved in $OutDir"
}
if ($gracefulObserved -and $null -ne $record.exit_code -and $record.exit_code -ne 0) {
    throw "Graceful close produced nonzero exit code $($record.exit_code); evidence saved in $OutDir"
}
if ($record.Contains('capture_failure')) {
    throw "Vita3K capture failed: $($record.capture_failure); evidence saved in $OutDir"
}
if ($record.exited_before_cleanup -and $record.Contains('exit_code') -and
    $null -ne $record.exit_code -and $record.exit_code -ne 0) {
    throw "Vita3K exited unsuccessfully ($($record.exit_code)); evidence saved in $OutDir"
}
