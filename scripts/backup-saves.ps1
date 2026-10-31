param(
    [string]$Pref = "",
    [string]$BackupRoot = "",
    [string]$Restore = "",
    [switch]$Replace
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Pref) { $Pref = Join-Path $Root "build/vita3k-pref" }
$Pref = (Resolve-Path -LiteralPath $Pref).Path
$Savedata = Join-Path $Pref "ux0/data/mhgu-vita-port/savedata"
if (-not $BackupRoot) { $BackupRoot = Join-Path $Root "local/save-backups" }

function Write-Manifest([string]$Directory, [string]$Destination) {
    $files = @(Get-ChildItem -LiteralPath $Directory -Recurse -File | Sort-Object FullName)
    $entries = foreach ($file in $files) {
        $relative = $file.FullName.Substring($Directory.Length).TrimStart('\','/').Replace('\', '/')
        [ordered]@{
            path = $relative
            bytes = $file.Length
            sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $manifest = [ordered]@{
        schema = 1
        source = $Directory
        created = (Get-Date).ToString("o")
        files = @($entries)
    }
    $ManifestPath = Join-Path $Destination "manifest.json"
    [IO.File]::WriteAllText($ManifestPath, ($manifest | ConvertTo-Json -Depth 6),
        (New-Object Text.UTF8Encoding($false)))
    return $ManifestPath
}

if ($Restore) {
    $Restore = (Resolve-Path -LiteralPath $Restore).Path
    $ManifestPath = Join-Path $Restore "manifest.json"
    if (-not (Test-Path -LiteralPath $ManifestPath -PathType Leaf)) {
        throw "Backup manifest is missing: $ManifestPath"
    }
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    function Resolve-BackupFile([string]$Root, [string]$Relative) {
        $relativePath = $Relative.Replace('/', '\')
        $direct = Join-Path $Root $relativePath
        if (Test-Path -LiteralPath $direct -PathType Leaf) { return $direct }
        return (Join-Path (Join-Path $Root "savedata") $relativePath)
    }
    foreach ($entry in $manifest.files) {
        $source = Resolve-BackupFile $Restore ([string]$entry.path)
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "Backup file is missing: $source"
        }
        $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne $entry.sha256) { throw "Backup file hash mismatch: $source" }
    }
    if (Test-Path -LiteralPath $Savedata) {
        if (-not $Replace) {
            throw "Current savedata exists. Re-run with -Replace to move it aside and restore."
        }
        $aside = "$Savedata.pre-restore-" + (Get-Date -Format "yyyyMMdd-HHmmss")
        Move-Item -LiteralPath $Savedata -Destination $aside
        Write-Host "Current savedata moved to: $aside"
    }
    New-Item -ItemType Directory -Force -Path $Savedata | Out-Null
    foreach ($entry in $manifest.files) {
        $source = Resolve-BackupFile $Restore ([string]$entry.path)
        $destination = Join-Path $Savedata ([string]$entry.path).Replace('/', '\')
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
        Copy-Item -LiteralPath $source -Destination $destination -Force
    }
    Write-Host "Savedata restored from: $Restore"
    return
}

if (-not (Test-Path -LiteralPath $Savedata -PathType Container)) {
    throw "No savedata directory to back up: $Savedata"
}
New-Item -ItemType Directory -Force -Path $BackupRoot | Out-Null
$destination = Join-Path $BackupRoot (Get-Date -Format "yyyyMMdd-HHmmss")
if (Test-Path -LiteralPath $destination) {
    $destination = Join-Path $BackupRoot (Get-Date -Format "yyyyMMdd-HHmmss-fff")
}
New-Item -ItemType Directory -Force -Path $destination | Out-Null
Copy-Item -LiteralPath $Savedata -Destination (Join-Path $destination "savedata") -Recurse
$ManifestPath = Write-Manifest (Join-Path $destination "savedata") $destination
Write-Host "Savedata backed up to: $destination"
Write-Host "Manifest: $ManifestPath"
