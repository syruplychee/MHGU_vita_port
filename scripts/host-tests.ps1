param(
    [string]$BuildDir = "",
    [string]$Python = $env:MHGU_PYTHON,
    [string]$CMake = "",
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build/host-tests" }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)

if (-not $Python) {
    foreach ($candidate in @("python3", "python")) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) { $Python = $command.Source; break }
    }
}
if (-not $CMake) {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { $CMake = $command.Source }
}
if (-not $CMake) {
    $local = Join-Path (Split-Path -Parent $Root) "tools/cmake/bin/cmake.exe"
    if (Test-Path -LiteralPath $local) { $CMake = $local }
}
if (-not $CMake) { throw "CMake 3.20+ was not found." }
$CMake = (Resolve-Path -LiteralPath $CMake).Path

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
$tmp = Join-Path $BuildDir "tmp"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TMP = $tmp
$env:TEMP = $tmp

& $CMake -S (Join-Path $Root "tests") -B $BuildDir
if ($LASTEXITCODE -ne 0) { throw "Host test configure failed." }
& $CMake --build $BuildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "Host test build failed." }
$ctestExe = Join-Path (Split-Path -Parent $CMake) "ctest.exe"
$ctest = if (Test-Path -LiteralPath $ctestExe) { $ctestExe } else { (Join-Path (Split-Path -Parent $CMake) "ctest") }
& $ctest --test-dir $BuildDir -C $Config --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "Host tests failed." }
