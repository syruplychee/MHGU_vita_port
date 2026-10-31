param([string]$Destination='')
$ErrorActionPreference='Stop'
$root=(Resolve-Path "$PSScriptRoot/../..").Path
# An explicit environment variable always wins, so a developer can point at any
# compatible Vita3K build without editing a tracked file.
if (-not $Destination -and $env:MHGU_VITA3K) {
    $selectedExe=[IO.Path]::GetFullPath($env:MHGU_VITA3K)
    if (-not (Test-Path -LiteralPath $selectedExe -PathType Leaf)) {
        throw "MHGU_VITA3K does not exist: $selectedExe"
    }
    Write-Output $selectedExe
    return
}
# A local selection file is optional and intentionally ignored by Git.
$selection=Join-Path $root 'config/vita3k-runtime.json'
if (-not $Destination -and (Test-Path -LiteralPath $selection)) {
    $selected=Get-Content -LiteralPath $selection -Raw | ConvertFrom-Json
    $selectedExe=Join-Path $root $selected.emulator
    if (-not (Test-Path -LiteralPath $selectedExe -PathType Leaf)) {
        throw "Selected Vita3K runtime is missing: $selectedExe; restore it or select an explicit -Emulator"
    }
    Write-Output (Resolve-Path -LiteralPath $selectedExe).Path
    return
}
if(-not $Destination){$Destination=Join-Path $root 'build/vita3k-runtime'}
$Destination=[IO.Path]::GetFullPath($Destination)
$build=[IO.Path]::GetFullPath((Join-Path $root 'build'))
if(-not $Destination.StartsWith($build+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){
    throw 'Private runtime must remain inside MHGU_vita_port/build'
}
$source=if ($env:MHGU_VITA3K_SOURCE) { (Resolve-Path -LiteralPath $env:MHGU_VITA3K_SOURCE).Path } else { (Resolve-Path "$root/../vita3k").Path }
New-Item -ItemType Directory -Force $Destination | Out-Null
# Hardlinks share immutable tool bytes, not config/cache/log state. UI assets
# are read-only directory junctions. The original emulator tree is untouched.
foreach($file in Get-ChildItem $source -File | Where-Object Extension -In '.exe','.dll'){
    $target=Join-Path $Destination $file.Name
    if(-not(Test-Path $target)){New-Item -ItemType HardLink -Path $target -Value $file.FullName | Out-Null}
}
foreach($name in @('data','icons','plugins','shaders-builtin','translations')){
    $target=Join-Path $Destination $name
    if((Test-Path "$source/$name") -and -not(Test-Path $target)){
        New-Item -ItemType Junction -Path $target -Target "$source/$name" | Out-Null
    }
}
Write-Output (Join-Path $Destination 'Vita3K.exe')
