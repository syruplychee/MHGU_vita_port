# Host regression for scripts/set-performance-profile.ps1.
#
# The real script is dot-sourced on throwaway runtime trees. Get-Process is
# shadowed only inside this test so both the running-Vita3K refusal and the
# write path are deterministic on an idle or busy host. No real emulator is
# started and no user data is touched.
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ProfileScript = Join-Path $Root "scripts/set-performance-profile.ps1"
$ConfigPath = Join-Path $Root "config/performance-profiles.json"
$BackupRoot = Join-Path $Root "local/performance-profile-backups"
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:MockVita3K = $false
$script:MockExited = $false
$script:TestBackups = @()

function Get-Process {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)][string[]]$Name,
        [int[]]$Id
    )
    if ($Name -and ($Name -ccontains "Vita3K")) {
        if ($script:MockVita3K) {
            return [pscustomobject]@{
                Id = 4242; ProcessName = "Vita3K"; HasExited = $script:MockExited
            }
        }
        return @()
    }
    return Microsoft.PowerShell.Management\Get-Process @PSBoundParameters
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "performance_profile test: $Message" }
}

function Write-TextFile([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)
}

function New-FakeRuntime([string]$Path, [bool]$WithFont) {
    New-Item -ItemType Directory -Force -Path $Path | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $Path "romfs-tree") | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $Path "romfs.index"), [byte[]](1, 2, 3))
    [IO.File]::WriteAllBytes((Join-Path $Path "romfs.map"), [byte[]](4, 5, 6))
    New-Item -ItemType Directory -Force -Path (Join-Path $Path "savedata/sce_sys") | Out-Null
    [IO.File]::WriteAllBytes((Join-Path $Path "savedata/sce_sys/slot0.bin"),
        [byte[]](9, 8, 7, 6))
    [IO.File]::WriteAllBytes((Join-Path $Path "eboot.bin"), [byte[]](0xAA, 0xBB))
    Write-TextFile (Join-Path $Path "unknown-marker.enable") "keep-unknown"
    # Pre-existing managed files so backup and rewrite paths are exercised.
    [IO.File]::WriteAllBytes((Join-Path $Path "pixel-font.enable"), [byte[]]@())
    Write-TextFile (Join-Path $Path "title-texture-heap-256.enable") "old-256"
    Write-TextFile (Join-Path $Path "guest-heap-limit-mib.txt") "1895`n"
    Write-TextFile (Join-Path $Path "render-resolution.txt") "360p`n"
    if ($WithFont) {
        $font = Join-Path $Path "font-native"
        New-Item -ItemType Directory -Force -Path (Join-Path $font "overlay") | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $font "romfs.index"), [byte[]](1, 1))
        [IO.File]::WriteAllBytes((Join-Path $font "romfs.map"), [byte[]](2, 2))
        Write-TextFile (Join-Path $font "overlay/font.bin") "font"
    }
}

function Get-TreeMap([string]$Path) {
    $map = @{}
    Get-ChildItem -LiteralPath $Path -Recurse -File -Force | ForEach-Object {
        $relative = $_.FullName.Substring($Path.Length).TrimStart('\', '/').Replace('\', '/')
        $map[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
    }
    return $map
}

function Get-DirectoryList([string]$Path) {
    return @(Get-ChildItem -LiteralPath $Path -Recurse -Directory -Force |
        ForEach-Object {
            $_.FullName.Substring($Path.Length).TrimStart('\', '/').Replace('\', '/')
        } | Sort-Object)
}

function Assert-TreeUnchanged([string]$Runtime, $Before, [string]$Context) {
    $after = Get-TreeMap $Runtime
    Assert-True ($after.Count -eq $Before.Count) "$Context changed the file count"
    foreach ($name in $Before.Keys) {
        Assert-True ($after.ContainsKey($name)) "$Context removed $name"
        Assert-True ($after[$name] -eq $Before[$name]) "$Context modified $name"
    }
}

function Get-ChangedNames($Before, $After) {
    $names = @{}
    foreach ($name in $Before.Keys) {
        if (-not $After.ContainsKey($name) -or $After[$name] -ne $Before[$name]) {
            $names[$name] = $true
        }
    }
    foreach ($name in $After.Keys) {
        if (-not $Before.ContainsKey($name)) { $names[$name] = $true }
    }
    return @($names.Keys)
}

function Get-BackupNames {
    if (-not (Test-Path -LiteralPath $BackupRoot -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $BackupRoot -Directory | ForEach-Object { $_.Name })
}

function Invoke-Profile([string]$ProfileName, [string]$Runtime, [bool]$DryRun) {
    try {
        $output = . $ProfileScript -Profile $ProfileName -RuntimeRoot $Runtime `
            -DryRun:$DryRun
        if (-not $DryRun) {
            $receipt = (($output | ForEach-Object { "$_" }) -join "`n") |
                ConvertFrom-Json
            $script:TestBackups += $receipt.backupDirectory
        }
        return [pscustomobject]@{
            Ok = $true
            Output = (($output | ForEach-Object { "$_" }) -join "`n")
            Error = $null
        }
    } catch {
        return [pscustomobject]@{
            Ok = $false
            Output = $null
            Error = $_.Exception.Message
        }
    }
}

function Assert-ManagedSetUnchanged([string]$Runtime, $Before) {
    $after = Get-TreeMap $Runtime
    $allowed = @{}
    $config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
    foreach ($name in @($config.markers)) { $allowed[$name] = $true }
    foreach ($name in @($config.text_files)) { $allowed[$name] = $true }
    $allowed["performance-profile.json"] = $true
    foreach ($name in @(Get-ChangedNames $Before $after)) {
        Assert-True ($allowed.ContainsKey($name)) `
            "profile script modified an unmanaged file: $name"
    }
    return $after
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) `
    ("mhgu-performance-profile-" + [guid]::NewGuid().ToString("N"))
$beforeBackups = @(Get-BackupNames)
try {
    New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
    $runtime = Join-Path $tempRoot "runtime"
    New-FakeRuntime $runtime $true

    # 1. A live Vita3K process refuses the write before any backup or change.
    $script:MockVita3K = $true
    $before = Get-TreeMap $runtime
    $busy = Invoke-Profile "compact" $runtime $false
    Assert-True (-not $busy.Ok) "compact should fail while Vita3K is running"
    Assert-True ($busy.Error -match "Vita3K is running") `
        "busy-host error did not identify Vita3K: $($busy.Error)"
    Assert-TreeUnchanged $runtime $before "busy-host refusal"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $runtime "performance-profile.json"))) `
        "busy-host refusal wrote a receipt"
    Assert-True (@(Get-BackupNames).Count -eq $beforeBackups.Count) `
        "busy-host refusal created a backup"
    $script:MockVita3K = $false

    # 2. DryRun validates the tree but writes nothing at all.
    $before = Get-TreeMap $runtime
    $beforeDirectories = (Get-DirectoryList $runtime) -join "`n"
    $dry = Invoke-Profile "compact" $runtime $true
    Assert-True ($dry.Ok) "compact dry-run failed: $($dry.Error)"
    Assert-TreeUnchanged $runtime $before "compact dry-run"
    Assert-True (((Get-DirectoryList $runtime) -join "`n") -ceq $beforeDirectories) `
        "compact dry-run changed directories"
    Assert-True (@(Get-BackupNames).Count -eq $beforeBackups.Count) `
        "compact dry-run created a backup"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $runtime "performance-profile.json"))) `
        "compact dry-run wrote a receipt"

    # recommended resolves to the configured candidate (compact today).
    $recommended = Invoke-Profile "recommended" $runtime $true
    Assert-True ($recommended.Ok) "recommended dry-run failed: $($recommended.Error)"
    $dryJson = $recommended.Output | ConvertFrom-Json
    Assert-True ($dryJson.profile -eq "compact") `
        "recommended did not resolve to compact"

    # 3. A missing font package is rejected before any write.
    $noFont = Join-Path $tempRoot "runtime-no-font"
    New-FakeRuntime $noFont $false
    $noFontBefore = Get-TreeMap $noFont
    $missing = Invoke-Profile "compact" $noFont $false
    Assert-True (-not $missing.Ok) "compact without font-native should fail"
    Assert-True ($missing.Error -match "requires font-native") `
        "missing font error did not explain the requirement: $($missing.Error)"
    Assert-TreeUnchanged $noFont $noFontBefore "missing-font refusal"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $noFont "performance-profile.json"))) `
        "missing-font refusal wrote a receipt"
    Assert-True (@(Get-BackupNames).Count -eq $beforeBackups.Count) `
        "missing-font refusal created a backup"

    # 3b. A font package with a missing map is also rejected before writes.
    $missingMap = Join-Path $tempRoot "runtime-missing-map"
    New-FakeRuntime $missingMap $true
    Remove-Item -LiteralPath (Join-Path $missingMap "font-native/romfs.map") -Force
    $missingMapBefore = Get-TreeMap $missingMap
    $badMap = Invoke-Profile "compact" $missingMap $false
    Assert-True (-not $badMap.Ok) "missing font ROMFS map should fail"
    Assert-TreeUnchanged $missingMap $missingMapBefore "missing-map refusal"
    Assert-True (@(Get-BackupNames).Count -eq $beforeBackups.Count) `
        "missing-map refusal created a backup"

    # 4. Apply compact: only managed files change; originals are backed up.
    $before = Get-TreeMap $runtime
    $applied = Invoke-Profile "compact" $runtime $false
    Assert-True ($applied.Ok) "compact apply failed: $($applied.Error)"
    $after = Assert-ManagedSetUnchanged $runtime $before

    $receiptPath = Join-Path $runtime "performance-profile.json"
    Assert-True (Test-Path -LiteralPath $receiptPath -PathType Leaf) "receipt missing"
    $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
    Assert-True ($receipt.profile -eq "compact") "receipt profile mismatch"
    $backupDir = [string]$receipt.backupDirectory
    Assert-True (Test-Path -LiteralPath $backupDir -PathType Container) `
        "backup directory missing: $backupDir"
    Assert-True (Test-Path -LiteralPath (Join-Path $backupDir "manifest.json") -PathType Leaf) `
        "backup manifest missing"
    Assert-True ((Get-Content -LiteralPath (Join-Path $backupDir "guest-heap-limit-mib.txt") -Raw) -ceq "1895`n") `
        "heap original was not backed up"
    Assert-True ((Get-Content -LiteralPath (Join-Path $backupDir "render-resolution.txt") -Raw) -ceq "360p`n") `
        "render-resolution original was not backed up"
    Assert-True ((Get-Content -LiteralPath (Join-Path $backupDir "title-texture-heap-256.enable") -Raw) -ceq "old-256") `
        "256 marker original was not backed up"
    Assert-True ((Get-Item -LiteralPath (Join-Path $backupDir "pixel-font.enable")).Length -eq 0) `
        "empty marker original was not backed up"

    $present = @(
        "pixel-font.enable", "pixel-font-native.enable",
        "work-log-reduce.enable",
        "title-texture-heap-512.enable", "gpu-present.enable",
        "bounded-batch.enable"
    )
    foreach ($name in $present) {
        Assert-True (Test-Path -LiteralPath (Join-Path $runtime $name) -PathType Leaf) `
            "compact should create $name"
    }
    $absent = @(
        "engine-full.enable", "engine-full-ab.enable", "engine-full-fast.enable",
        "engine-layout.enable", "game-cost.enable", "frame-cost.enable",
        "play-perf.enable", "resource-lifecycle.enable", "pixel-font-trace.enable",
        "texture-pipeline.disable", "texture-pool-128.enable",
        "title-texture-heap-256.enable", "native-movie.enable",
        "texture-idle-recycle.enable",
        "scene-pipeline.disable", "clear-fusion.disable"
    )
    foreach ($name in $absent) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $runtime $name))) `
            "compact should remove $name"
    }
    Assert-True (([IO.File]::ReadAllText((Join-Path $runtime "guest-heap-limit-mib.txt"))) -ceq "1664`n") `
        "compact heap value mismatch"
    Assert-True (([IO.File]::ReadAllText((Join-Path $runtime "render-resolution.txt"))) -ceq "540p`n") `
        "compact render resolution mismatch"
    Assert-True ($after.ContainsKey("savedata/sce_sys/slot0.bin")) "savedata disappeared"
    Assert-True ($after.ContainsKey("eboot.bin")) "eboot disappeared"
    Assert-True ($after.ContainsKey("unknown-marker.enable")) "unknown file disappeared"

    # 5. Baseline restores the documented comparison configuration.
    $before = Get-TreeMap $runtime
    $baseline = Invoke-Profile "baseline" $runtime $false
    Assert-True ($baseline.Ok) "baseline apply failed: $($baseline.Error)"
    $after = Assert-ManagedSetUnchanged $runtime $before
    foreach ($name in @("title-texture-heap-512.enable", "gpu-present.enable",
                        "bounded-batch.enable")) {
        Assert-True (Test-Path -LiteralPath (Join-Path $runtime $name) -PathType Leaf) `
            "baseline should create $name"
    }
    foreach ($name in @("pixel-font.enable", "pixel-font-native.enable",
                        "work-log-reduce.enable",
                        "texture-pool-128.enable", "texture-pipeline.disable",
                        "texture-idle-recycle.enable", "title-texture-heap-256.enable")) {
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $runtime $name))) `
            "baseline should remove $name"
    }
    Assert-True (([IO.File]::ReadAllText((Join-Path $runtime "guest-heap-limit-mib.txt"))) -ceq "1664`n") `
        "baseline heap value mismatch"
    Assert-True ($after.ContainsKey("savedata/sce_sys/slot0.bin")) "savedata disappeared"
    Assert-True ($after.ContainsKey("unknown-marker.enable")) "unknown file disappeared"

    # 6. Expanded adds the 128 MiB budget to compact.
    $before = Get-TreeMap $runtime
    $expanded = Invoke-Profile "expanded" $runtime $false
    Assert-True ($expanded.Ok) "expanded apply failed: $($expanded.Error)"
    $after = Assert-ManagedSetUnchanged $runtime $before
    foreach ($name in @("pixel-font.enable", "pixel-font-native.enable",
                        "work-log-reduce.enable",
                        "texture-pool-128.enable", "title-texture-heap-512.enable",
                        "gpu-present.enable", "bounded-batch.enable")) {
        Assert-True (Test-Path -LiteralPath (Join-Path $runtime $name) -PathType Leaf) `
            "expanded should create $name"
    }
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $runtime "texture-pipeline.disable"))) `
        "expanded must not disable the deep pipeline"
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $runtime "title-texture-heap-256.enable"))) `
        "expanded must not enable the rejected 256 MiB subheap"
    Assert-True (([IO.File]::ReadAllText((Join-Path $runtime "guest-heap-limit-mib.txt"))) -ceq "1664`n") `
        "expanded heap value mismatch"
    Assert-True ($after.ContainsKey("savedata/sce_sys/slot0.bin")) "savedata disappeared"
    Assert-True ($after.ContainsKey("eboot.bin")) "eboot disappeared"
    Assert-True ($after.ContainsKey("unknown-marker.enable")) "unknown file disappeared"

    # Windows can retain an already-exited Vita3K process entry. Only a live
    # process owns the runtime; an exited entry must not block applying it.
    $script:MockVita3K = $true
    $script:MockExited = $true
    $terminated = Invoke-Profile "baseline" $runtime $false
    Assert-True ($terminated.Ok) "terminated process entry blocked the profile"

    Write-Output "performance_profile: all checks passed"
} finally {
    $tempFull = [IO.Path]::GetFullPath($tempRoot)
    $tempParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/') + '\'
    if (-not $tempFull.StartsWith($tempParent, [StringComparison]::OrdinalIgnoreCase) -or
        (Split-Path -Leaf $tempFull) -notlike 'mhgu-performance-profile-*') {
        throw 'Test cleanup path is outside its temporary root'
    }
    if (Test-Path -LiteralPath $tempRoot) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force
    }
    $backupPrefix = [IO.Path]::GetFullPath($BackupRoot).TrimEnd('\', '/') + '\'
    foreach ($directory in $script:TestBackups) {
        $full = [IO.Path]::GetFullPath($directory)
        if (-not $full.StartsWith($backupPrefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Test backup cleanup path is outside the project backup root'
        }
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}
