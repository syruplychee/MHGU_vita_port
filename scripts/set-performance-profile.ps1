# Apply one performance profile to a Vita3K runtime data directory.
#
# The profile data lives only in config/performance-profiles.json. This script
# validates every profile and every managed name before touching the runtime
# root, backs up the original bytes of every managed file, and changes only the
# markers and text files listed in that file. Saves, eboot, fonts, logs and
# unknown files are never read for modification or deleted.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet("recommended", "baseline", "compact", "expanded")]
    [string]$Profile,
    [string]$RuntimeRoot = "",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ConfigRelative = "config/performance-profiles.json"
$ConfigPath = Join-Path $Root $ConfigRelative

function Fail([string]$Message) {
    throw "set-performance-profile: $Message"
}

function Get-RequiredProperty($Object, [string]$Name, [string]$Context) {
    if ($null -eq $Object) { Fail "$Context is null" }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { Fail "$Context is missing '$Name'" }
    return $property.Value
}

function Test-SafeName([string]$Name, [string]$Context) {
    if ([string]::IsNullOrEmpty($Name)) { Fail "$Context has an empty name" }
    if ($Name -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]*$') {
        Fail "$Context '$Name' is not a safe basename"
    }
    if ($Name -ceq "performance-profile.json") {
        Fail "$Context '$Name' collides with the result receipt"
    }
}

function Resolve-SafeChild([string]$Parent, [string]$Name, [string]$Context) {
    Test-SafeName $Name $Context
    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/')
    $childFull = [IO.Path]::GetFullPath((Join-Path $parentFull $Name))
    $prefix = $parentFull + [IO.Path]::DirectorySeparatorChar
    if (-not $childFull.StartsWith($prefix, [StringComparison]::Ordinal)) {
        Fail "$Context '$Name' escapes '$parentFull'"
    }
    return $childFull
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Test-BytesEqual([byte[]]$Left, [byte[]]$Right) {
    if ($Left.Length -ne $Right.Length) { return $false }
    for ($index = 0; $index -lt $Left.Length; ++$index) {
        if ($Left[$index] -ne $Right[$index]) { return $false }
    }
    return $true
}

function Write-JsonFile([string]$Path, $Value) {
    $json = $Value | ConvertTo-Json -Depth 8
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($Path, $json + "`n", $encoding)
}

function Assert-NoVita3K {
    $running = @(Get-Process -Name Vita3K -ErrorAction SilentlyContinue |
        Where-Object { -not $_.HasExited })
    if ($running.Count -gt 0) {
        $ids = ($running | ForEach-Object { $_.Id }) -join ","
        Fail "Vita3K is running (pid $ids); close it before writing a profile"
    }
}

# ---- Read and validate the complete configuration ------------------------

if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
    Fail "profile configuration is missing: $ConfigRelative"
}
try {
    $config = Get-Content -LiteralPath $ConfigPath -Raw | ConvertFrom-Json
} catch {
    Fail "invalid profile JSON: $($_.Exception.Message)"
}
$configHash = Get-Sha256 $ConfigPath

$version = Get-RequiredProperty $config "version" "config"
if ($version -is [bool] -or
    -not ($version -is [int] -or $version -is [long] -or
          $version -is [int16] -or $version -is [byte])) {
    Fail "config.version must be an integer"
}
if ([long]$version -ne 1) { Fail "config.version must be 1" }

$recommended = Get-RequiredProperty $config "recommended" "config"
if (-not ($recommended -is [string])) { Fail "config.recommended must be a string" }

$defaultRuntimeRoot = Get-RequiredProperty $config "default_runtime_root" "config"
if (-not ($defaultRuntimeRoot -is [string]) -or
    [string]::IsNullOrEmpty($defaultRuntimeRoot) -or
    [IO.Path]::IsPathRooted($defaultRuntimeRoot)) {
    Fail "config.default_runtime_root must be a non-empty relative path"
}

$fontConfig = Get-RequiredProperty $config "font_native" "config"
if (-not ($fontConfig -is [System.Management.Automation.PSCustomObject])) {
    Fail "config.font_native must be an object"
}
$fontDirectory = Get-RequiredProperty $fontConfig "directory" "config.font_native"
$fontIndex = Get-RequiredProperty $fontConfig "index" "config.font_native"
$fontMap = Get-RequiredProperty $fontConfig "map" "config.font_native"
$fontOverlay = Get-RequiredProperty $fontConfig "overlay" "config.font_native"
foreach ($pair in @(
        @($fontDirectory, "font_native.directory"),
        @($fontIndex, "font_native.index"),
        @($fontMap, "font_native.map"),
        @($fontOverlay, "font_native.overlay"))) {
    if (-not ($pair[0] -is [string])) { Fail "config.$($pair[1]) must be a string" }
    Test-SafeName $pair[0] "config.$($pair[1])"
}

$rawMarkerNames = Get-RequiredProperty $config "markers" "config"
$rawTextNames = Get-RequiredProperty $config "text_files" "config"
if (-not ($rawMarkerNames -is [System.Array])) {
    Fail "config.markers must be an array"
}
if (-not ($rawTextNames -is [System.Array])) {
    Fail "config.text_files must be an array"
}
$markerNames = @($rawMarkerNames)
$textNames = @($rawTextNames)
if ($markerNames.Count -eq 0) { Fail "config.markers must not be empty" }
if ($textNames.Count -eq 0) { Fail "config.text_files must not be empty" }
foreach ($marker in $markerNames) {
    if (-not ($marker -is [string])) { Fail "config.markers contains a non-string" }
    Test-SafeName $marker "config.markers"
}
foreach ($textFile in $textNames) {
    if (-not ($textFile -is [string])) { Fail "config.text_files contains a non-string" }
    Test-SafeName $textFile "config.text_files"
}
foreach ($marker in $markerNames) {
    if (@($markerNames | Where-Object { $_ -ceq $marker }).Count -ne 1) {
        Fail "config.markers contains duplicate '$marker'"
    }
    if ($textNames -ccontains $marker) {
        Fail "'$marker' is both a marker and a text file"
    }
}
foreach ($textFile in $textNames) {
    if (@($textNames | Where-Object { $_ -ceq $textFile }).Count -ne 1) {
        Fail "config.text_files contains duplicate '$textFile'"
    }
}

$profilesObject = Get-RequiredProperty $config "profiles" "config"
if (-not ($profilesObject -is [System.Management.Automation.PSCustomObject])) {
    Fail "config.profiles must be an object"
}
$profileNames = @($profilesObject.PSObject.Properties.Name)
foreach ($required in @("baseline", "compact", "expanded")) {
    if ($profileNames -cnotcontains $required) {
        Fail "config.profiles is missing '$required'"
    }
}
if ($profileNames -cnotcontains $recommended) {
    Fail "config.recommended '$recommended' is not a profile"
}

$profileValues = @{}
foreach ($profileName in $profileNames) {
    Test-SafeName $profileName "config.profiles"
    $entry = $profilesObject.PSObject.Properties[$profileName].Value
    if ($null -eq $entry -or -not ($entry -is [System.Management.Automation.PSCustomObject])) {
        Fail "profile '$profileName' must be an object"
    }
    $description = $entry.PSObject.Properties["description"]
    if ($null -ne $description -and -not ($description.Value -is [string])) {
        Fail "profile '$profileName'.description must be a string"
    }
    $requires = Get-RequiredProperty $entry "requires_font_native" "profile '$profileName'"
    if (-not ($requires -is [bool])) {
        Fail "profile '$profileName'.requires_font_native must be a boolean"
    }

    $markerMap = Get-RequiredProperty $entry "markers" "profile '$profileName'"
    if (-not ($markerMap -is [System.Management.Automation.PSCustomObject])) {
        Fail "profile '$profileName'.markers must be an object"
    }
    $mapNames = @($markerMap.PSObject.Properties.Name)
    if ($mapNames.Count -ne $markerNames.Count) {
        Fail "profile '$profileName'.markers must list exactly $($markerNames.Count) managed markers"
    }
    $markerValues = @{}
    foreach ($marker in $markerNames) {
        $property = $markerMap.PSObject.Properties[$marker]
        if ($null -eq $property) {
            Fail "profile '$profileName' is missing marker '$marker'"
        }
        if (-not ($property.Value -is [bool])) {
            Fail "profile '$profileName'.markers['$marker'] must be a boolean"
        }
        $markerValues[$marker] = [bool]$property.Value
    }
    foreach ($name in $mapNames) {
        if ($markerNames -cnotcontains $name) {
            Fail "profile '$profileName' has unknown marker '$name'"
        }
    }

    $textMap = Get-RequiredProperty $entry "text_files" "profile '$profileName'"
    if (-not ($textMap -is [System.Management.Automation.PSCustomObject])) {
        Fail "profile '$profileName'.text_files must be an object"
    }
    $textMapNames = @($textMap.PSObject.Properties.Name)
    if ($textMapNames.Count -ne $textNames.Count) {
        Fail "profile '$profileName'.text_files must list exactly $($textNames.Count) managed files"
    }
    $textValues = @{}
    foreach ($textFile in $textNames) {
        $property = $textMap.PSObject.Properties[$textFile]
        if ($null -eq $property) {
            Fail "profile '$profileName' is missing text file '$textFile'"
        }
        if (-not ($property.Value -is [string])) {
            Fail "profile '$profileName'.text_files['$textFile'] must be a string"
        }
        $textValues[$textFile] = [string]$property.Value
    }
    foreach ($name in $textMapNames) {
        if ($textNames -cnotcontains $name) {
            Fail "profile '$profileName' has unknown text file '$name'"
        }
    }

    $profileValues[$profileName] = @{
        Markers = $markerValues
        TextFiles = $textValues
        RequiresFontNative = [bool]$requires
    }
}

$selectedName = if ($Profile -eq "recommended") { [string]$recommended } else { $Profile }
if (-not $profileValues.ContainsKey($selectedName)) {
    Fail "selected profile '$selectedName' does not exist"
}
$selected = $profileValues[$selectedName]

# ---- Resolve and validate the target before backing up or writing --------

if (-not $RuntimeRoot) {
    $RuntimeRoot = Join-Path $Root $defaultRuntimeRoot
}
$runtimeFull = [IO.Path]::GetFullPath($RuntimeRoot)
if (-not (Test-Path -LiteralPath $runtimeFull -PathType Container)) {
    Fail "runtime root is not a directory: $runtimeFull"
}

if ($selected.RequiresFontNative) {
    $fontDir = Resolve-SafeChild $runtimeFull $fontDirectory "font-native directory"
    if (-not (Test-Path -LiteralPath $fontDir -PathType Container)) {
        Fail "profile '$selectedName' requires $fontDirectory/ with romfs.index, romfs.map and overlay/"
    }
    foreach ($pair in @(
            @($fontIndex, "font-native index"),
            @($fontMap, "font-native map"))) {
        $path = Resolve-SafeChild $fontDir $pair[0] $pair[1]
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            Fail "profile '$selectedName' requires $fontDirectory/$($pair[0])"
        }
    }
    $overlayPath = Resolve-SafeChild $fontDir $fontOverlay "font-native overlay"
    if (-not (Test-Path -LiteralPath $overlayPath -PathType Container)) {
        Fail "profile '$selectedName' requires $fontDirectory/$fontOverlay/"
    }
}

$encoding = New-Object System.Text.UTF8Encoding($false)
$markerState = @{}
$textState = @{}
foreach ($marker in $markerNames) {
    $path = Resolve-SafeChild $runtimeFull $marker "managed marker"
    if (Test-Path -LiteralPath $path -PathType Container) {
        Fail "managed marker path is a directory: $path"
    }
    $markerState[$marker] = @{
        Path = $path
        Exists = Test-Path -LiteralPath $path -PathType Leaf
        Desired = [bool]$selected.Markers[$marker]
    }
}
foreach ($textFile in $textNames) {
    $path = Resolve-SafeChild $runtimeFull $textFile "managed text file"
    if (Test-Path -LiteralPath $path -PathType Container) {
        Fail "managed text file path is a directory: $path"
    }
    $exists = Test-Path -LiteralPath $path -PathType Leaf
    $desiredBytes = $encoding.GetBytes([string]$selected.TextFiles[$textFile])
    $same = $false
    if ($exists) {
        $same = Test-BytesEqual ([IO.File]::ReadAllBytes($path)) $desiredBytes
    }
    $textState[$textFile] = @{
        Path = $path
        Exists = $exists
        DesiredBytes = $desiredBytes
        Same = $same
    }
}

$changes = @()
foreach ($marker in $markerNames) {
    $state = $markerState[$marker]
    $action = "keep"
    if ($state.Desired -and -not $state.Exists) { $action = "create" }
    elseif (-not $state.Desired -and $state.Exists) { $action = "remove" }
    $changes += [pscustomobject][ordered]@{
        kind = "marker"
        name = $marker
        action = $action
        existed_before = $state.Exists
        desired = $state.Desired
    }
}
foreach ($textFile in $textNames) {
    $state = $textState[$textFile]
    $action = "keep"
    if (-not $state.Exists) { $action = "create" }
    elseif (-not $state.Same) { $action = "write" }
    $changes += [pscustomobject][ordered]@{
        kind = "text"
        name = $textFile
        action = $action
        existed_before = $state.Exists
        desired = $true
    }
}
$plannedChanges = @($changes | Where-Object { $_.action -ne "keep" }).Count

if ($DryRun) {
    $summary = [ordered]@{
        dryRun = $true
        profile = $selectedName
        runtimeRoot = $runtimeFull
        plannedChanges = $plannedChanges
        changes = $changes
    }
    $summary | ConvertTo-Json -Depth 8
    return
}

Assert-NoVita3K

# ---- Back up every managed original before the first runtime write -------

$backupRoot = Join-Path $Root "local/performance-profile-backups"
New-Item -ItemType Directory -Force -Path $backupRoot | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$unique = [guid]::NewGuid().ToString("N").Substring(0, 8)
$backupDir = Join-Path $backupRoot "$stamp-$unique"
New-Item -ItemType Directory -Path $backupDir | Out-Null

$manifestFiles = @()
foreach ($marker in $markerNames) {
    $state = $markerState[$marker]
    $entry = [ordered]@{
        kind = "marker"
        name = $marker
        existed = $state.Exists
        bytes = $null
        sha256 = $null
    }
    if ($state.Exists) {
        Copy-Item -LiteralPath $state.Path -Destination (Join-Path $backupDir $marker)
        $entry.bytes = (Get-Item -LiteralPath $state.Path).Length
        $entry.sha256 = Get-Sha256 $state.Path
    }
    $manifestFiles += [pscustomobject]$entry
}
foreach ($textFile in $textNames) {
    $state = $textState[$textFile]
    $entry = [ordered]@{
        kind = "text"
        name = $textFile
        existed = $state.Exists
        bytes = $null
        sha256 = $null
    }
    if ($state.Exists) {
        Copy-Item -LiteralPath $state.Path -Destination (Join-Path $backupDir $textFile)
        $entry.bytes = (Get-Item -LiteralPath $state.Path).Length
        $entry.sha256 = Get-Sha256 $state.Path
    }
    $manifestFiles += [pscustomobject]$entry
}
$manifest = [ordered]@{
    schema = 1
    createdAtUtc = (Get-Date).ToUniversalTime().ToString("o")
    profile = $selectedName
    runtimeRoot = $runtimeFull
    config = $ConfigRelative
    configSha256 = $configHash
    files = $manifestFiles
}
Write-JsonFile (Join-Path $backupDir "manifest.json") $manifest

# Re-check immediately before the first write.
Assert-NoVita3K

# ---- Apply only the managed files ---------------------------------------

foreach ($marker in $markerNames) {
    $state = $markerState[$marker]
    if ($state.Desired) {
        if (-not $state.Exists) {
            [IO.File]::WriteAllBytes($state.Path, [byte[]]@())
        }
    } elseif ($state.Exists) {
        Remove-Item -LiteralPath $state.Path -Force
    }
}
foreach ($textFile in $textNames) {
    $state = $textState[$textFile]
    if (-not $state.Exists -or -not $state.Same) {
        [IO.File]::WriteAllBytes($state.Path, $state.DesiredBytes)
    }
}

$receiptPath = Join-Path $runtimeFull "performance-profile.json"
$receipt = [ordered]@{
    schema = 1
    profile = $selectedName
    appliedAtUtc = (Get-Date).ToUniversalTime().ToString("o")
    runtimeRoot = $runtimeFull
    config = $ConfigRelative
    configSha256 = $configHash
    backupDirectory = $backupDir
    changes = $changes
}
Write-JsonFile $receiptPath $receipt

$result = [ordered]@{
    dryRun = $false
    profile = $selectedName
    runtimeRoot = $runtimeFull
    plannedChanges = $plannedChanges
    backupDirectory = $backupDir
    receipt = $receiptPath
    changes = $changes
}
$result | ConvertTo-Json -Depth 8
