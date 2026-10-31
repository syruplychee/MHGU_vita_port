param(
    [string]$RomFs = $env:MHGU_ROMFS,
    [string]$RuntimeRoot = "",
    [string]$Python = $env:MHGU_PYTHON,
    [string]$ShaderPackage = "",
    # Optional writable directory for Python-generated metadata. Use this when
    # the project drive is protected against non-shell process writes.
    [string]$StagingRoot = "",
    [switch]$Force
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $RomFs) { throw "Set MHGU_ROMFS or pass -RomFs (the directory that contains nativeNX)." }
$RomFs = (Resolve-Path -LiteralPath $RomFs).Path
if (-not (Test-Path -LiteralPath (Join-Path $RomFs "nativeNX") -PathType Container)) {
    throw "RomFS root must contain nativeNX: $RomFs"
}
if (-not $RuntimeRoot) { $RuntimeRoot = Join-Path $Root "build/vita3k-pref/ux0/data/mhgu-vita-port" }
$RuntimeRoot = [IO.Path]::GetFullPath($RuntimeRoot)
New-Item -ItemType Directory -Force -Path $RuntimeRoot | Out-Null

if (-not $Python) {
    foreach ($candidate in @("python3", "python")) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) { $Python = $command.Source; break }
    }
}
if (-not $Python) { throw "Python 3.10+ was not found. Set MHGU_PYTHON or pass -Python." }

$generationRoot = $RuntimeRoot
if ($StagingRoot) {
    $generationRoot = [IO.Path]::GetFullPath($StagingRoot)
    New-Item -ItemType Directory -Force -Path $generationRoot | Out-Null
    # Give Python a temporary directory it can actually write to. This also
    # covers tools that create their own temporary tree.
    $env:TMP = $generationRoot
    $env:TEMP = $generationRoot
}

Write-Host "== RomFS metadata =="
& $Python (Join-Path $Root "tools/romfs/build_romfs_index.py") --tree $RomFs --output $generationRoot
if ($LASTEXITCODE -ne 0) { throw "RomFS metadata build failed." }
if ($generationRoot -ne $RuntimeRoot) {
    foreach ($name in @("romfs.index", "romfs.map", "romfs.json")) {
        $source = Join-Path $generationRoot $name
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $RuntimeRoot $name) -Force
        }
    }
}

$treeLink = Join-Path $RuntimeRoot "romfs-tree"
if (Test-Path -LiteralPath $treeLink) {
    $existing = (Get-Item -LiteralPath $treeLink).Target
    $existing = if ($existing) { [string]$existing } else { "" }
    if ($existing -and ([IO.Path]::GetFullPath($existing) -ne $RomFs)) {
        if (-not $Force) {
            throw "romfs-tree already points at $existing. Re-run with -Force to replace it."
        }
        Remove-Item -LiteralPath $treeLink -Force
    }
}
if (-not (Test-Path -LiteralPath $treeLink)) {
    New-Item -ItemType Junction -Path $treeLink -Target $RomFs | Out-Null
}

Write-Host "== Offline system data =="
$offlineSource = Join-Path $generationRoot "offline-ngword2.romfs"
& $Python (Join-Path $Root "tools/romfs/build_offline_system_data.py") --output $offlineSource
if ($LASTEXITCODE -ne 0) { throw "Offline system data build failed." }
if ($generationRoot -ne $RuntimeRoot) {
    Copy-Item -LiteralPath $offlineSource -Destination (Join-Path $RuntimeRoot "offline-ngword2.romfs") -Force
}

Copy-Item -LiteralPath (Join-Path $Root "config/render-resolution.txt") `
    -Destination (Join-Path $RuntimeRoot "render-resolution.txt") -Force

if ($ShaderPackage) {
    $ShaderPackage = (Resolve-Path -LiteralPath $ShaderPackage).Path
    foreach ($required in @("manifest.json", "shader-index.bin", "shader-resources.bin")) {
        if (-not (Test-Path -LiteralPath (Join-Path $ShaderPackage $required) -PathType Leaf)) {
            throw "Shader package is missing ${required}: $ShaderPackage"
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $ShaderPackage "shaders") -PathType Container)) {
        throw "Shader package is missing shaders/: $ShaderPackage"
    }
    $installed = Join-Path $RuntimeRoot "static-shaders"
    if (Test-Path -LiteralPath $installed) {
        if (-not $Force) {
            throw "static-shaders already exists: $installed. Re-run with -Force to replace it."
        }
        Remove-Item -LiteralPath $installed -Recurse -Force
    }
    Copy-Item -LiteralPath $ShaderPackage -Destination $installed -Recurse
    Write-Host "Shader package installed: $installed"
}

Write-Host ""
Write-Host "Runtime data ready: $RuntimeRoot"
Write-Host "  romfs.index / romfs.map / romfs-tree / offline-ngword2.romfs"
if (-not $ShaderPackage) {
    Write-Host "  static shader bank not installed; pass -ShaderPackage to install one."
}
