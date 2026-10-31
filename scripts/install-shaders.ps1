param(
    [Parameter(Mandatory = $true)][string]$PackageDir,
    [string]$RuntimeRoot = "",
    [string]$Python = $env:MHGU_PYTHON,
    [switch]$Force
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$PackageDir = (Resolve-Path -LiteralPath $PackageDir).Path
if (-not $RuntimeRoot) { $RuntimeRoot = Join-Path $Root "build/vita3k-pref/ux0/data/mhgu-vita-port" }
$RuntimeRoot = [IO.Path]::GetFullPath($RuntimeRoot)
if (-not $Python) {
    foreach ($candidate in @("python3", "python")) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) { $Python = $command.Source; break }
    }
}
if (-not $Python) { throw "Python 3.10+ was not found. Set MHGU_PYTHON or pass -Python." }

& $Python (Join-Path $Root "tools/shader/verify_static_package.py") $PackageDir
if ($LASTEXITCODE -ne 0) { throw "Static shader package failed verification." }

$target = Join-Path $RuntimeRoot "static-shaders"
if (Test-Path -LiteralPath $target) {
    if (-not $Force) { throw "static-shaders already exists: $target. Re-run with -Force." }
    Remove-Item -LiteralPath $target -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $RuntimeRoot | Out-Null
Copy-Item -LiteralPath $PackageDir -Destination $target -Recurse
Write-Host "Installed static shader source bank: $target"
Write-Host "run_gui_play.ps1 stages it into the compiled package directory recorded in build/vita/static-shader-package.json."
