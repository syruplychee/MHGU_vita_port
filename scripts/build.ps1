param(
    [string]$ExeFs = $env:MHGU_EXEFS,
    [string]$KubridgeLib = $env:MHGU_KUBRIDGE_LIB,
    [string]$BuildDir = "",
    [string]$StaticShaderDir = "ux0:data/mhgu-vita-port/static-shaders-current",
    [string]$Python = $env:MHGU_PYTHON,
    [string]$CMake = "",
    [string]$Ninja = $env:MHGU_NINJA,
    [switch]$NoBootstrap,
    [switch]$Clean
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $BuildDir) { $BuildDir = Join-Path $Root "build/vita" }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)

function Fail([string]$Message) { throw $Message }

if (-not $env:VITASDK) {
    Fail "VITASDK is not set. Install VitaSDK and point VITASDK at its root (see docs/02-dependencies.md)."
}
$toolchain = Join-Path $env:VITASDK "share/vita.toolchain.cmake"
if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
    Fail "VitaSDK toolchain file is missing: $toolchain"
}
if (-not $ExeFs) {
    Fail "Set MHGU_EXEFS or pass -ExeFs (see docs/03-game-inputs.md)."
}
$ExeFs = (Resolve-Path -LiteralPath $ExeFs).Path
foreach ($name in @("rtld", "main", "subsdk0", "sdk", "main.npdm")) {
    if (-not (Test-Path -LiteralPath (Join-Path $ExeFs $name) -PathType Leaf)) {
        Fail "Game ExeFS is missing ${name}: $ExeFs"
    }
}

if (-not $Python) {
    $candidates = @("python3", "python")
    foreach ($candidate in $candidates) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) { $Python = $command.Source; break }
    }
}
if (-not $Python) {
    Fail "Python 3.10+ was not found. Set MHGU_PYTHON or pass -Python."
}
$Python = (Resolve-Path -LiteralPath $Python).Path
& $Python -c "import sys; assert sys.version_info >= (3, 10)" 2>$null
if ($LASTEXITCODE -ne 0) { Fail "Python at $Python is older than 3.10." }
& $Python -c "import lz4.block" 2>$null
if ($LASTEXITCODE -ne 0) {
    Fail "Python package lz4 is missing. Run: `"$Python`" -m pip install -r tools/requirements.txt"
}

if (-not $CMake) {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { $CMake = $command.Source }
}
if (-not $CMake) {
    $local = Join-Path (Split-Path -Parent $Root) "tools/cmake/bin/cmake.exe"
    if (Test-Path -LiteralPath $local) { $CMake = $local }
}
if (-not $CMake) { Fail "CMake 3.24+ was not found. Set PATH or pass -CMake." }
$CMake = (Resolve-Path -LiteralPath $CMake).Path

if (-not $Ninja) {
    $command = Get-Command ninja -ErrorAction SilentlyContinue
    if ($command) { $Ninja = $command.Source }
}
if (-not $Ninja) {
    $local = Join-Path (Split-Path -Parent $Root) "tools/ninja/ninja.exe"
    if (Test-Path -LiteralPath $local) { $Ninja = $local }
}
if (-not $Ninja) { Fail "Ninja was not found. Set PATH or MHGU_NINJA." }
$Ninja = (Resolve-Path -LiteralPath $Ninja).Path

if (-not (Test-Path -LiteralPath $BuildDir)) {
    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
}
# The system temp directory is not writable in some hardened Windows setups.
# Keep compiler and CMake scratch files inside the repository build tree.
$tmp = Join-Path $BuildDir "tmp"
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
$env:TMP = $tmp
$env:TEMP = $tmp

if ($Clean) {
    if (Test-Path -LiteralPath (Join-Path $BuildDir "CMakeCache.txt")) {
        Remove-Item -LiteralPath (Join-Path $BuildDir "CMakeCache.txt") -Force
    }
    $cacheDir = Join-Path $BuildDir "CMakeFiles"
    if (Test-Path -LiteralPath $cacheDir) { Remove-Item -LiteralPath $cacheDir -Recurse -Force }
}

$configure = @(
    "-S", $Root, "-B", $BuildDir, "-G", "Ninja",
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
    "-DCMAKE_MAKE_PROGRAM=$Ninja",
    "-DPython3_EXECUTABLE=$Python",
    "-DVITA_PORT_EXEFS=$ExeFs",
    "-DVITA_PORT_STATIC_SHADER_PACKAGE_DIR=$StaticShaderDir",
    "-DVITA_PORT_BUILD_NATIVE_LOADER=ON",
    "-DVITA_PORT_NATIVE_SDK_DSO=ON"
)
if ($KubridgeLib) { $configure += "-DVITA_PORT_KUBRIDGE_LIBRARY=$KubridgeLib" }
if ($NoBootstrap) { $configure += "-DVITA_PORT_BUILD_BOOTSTRAP=OFF" }

Write-Host "== configure =="
& $CMake @configure
if ($LASTEXITCODE -ne 0) { Fail "CMake configure failed." }

Write-Host "== build VPK =="
& $CMake --build $BuildDir --target vita_port_native_loader.vpk
if ($LASTEXITCODE -ne 0) { Fail "VPK build failed." }

$vpk = Join-Path $BuildDir "vita_port_native_loader.vpk"
if (-not (Test-Path -LiteralPath $vpk -PathType Leaf)) { Fail "VPK was not produced: $vpk" }
$hash = (Get-FileHash -LiteralPath $vpk -Algorithm SHA256).Hash.ToLowerInvariant()
$self = Join-Path $BuildDir "vita_port_native_loader.self"
$selfHash = if (Test-Path -LiteralPath $self) { (Get-FileHash -LiteralPath $self -Algorithm SHA256).Hash.ToLowerInvariant() } else { "" }
Write-Host ""
Write-Host "VPK:    $vpk"
Write-Host "SHA256: $hash"
if ($selfHash) { Write-Host "SELF:   $self`nSHA256: $selfHash" }
Write-Host "Package runtime dir: $StaticShaderDir"
