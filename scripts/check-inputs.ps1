param(
    [string]$ExeFs = $env:MHGU_EXEFS,
    [string]$Nacp = $env:MHGU_NACP,
    [string]$Python = $env:MHGU_PYTHON,
    [string]$Output = ""
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $ExeFs) { throw "Set MHGU_EXEFS or pass -ExeFs." }
if (-not $Nacp) { throw "Set MHGU_NACP or pass -Nacp (control.nacp)." }
if (-not $Output) { $Output = Join-Path $Root "build/reports/input-check.json" }
if (-not $Python) {
    foreach ($candidate in @("python3", "python")) {
        $command = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($command) { $Python = $command.Source; break }
    }
}
if (-not $Python) { throw "Python 3.10+ was not found. Set MHGU_PYTHON or pass -Python." }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Output) | Out-Null
# Capture stdout and write the report from PowerShell. This keeps the check
# usable on machines where Python is not allowed to write to the project drive.
$json = & $Python (Join-Path $Root "tools/input/verify_input.py") `
    --exefs $ExeFs --nacp $Nacp 2>&1
$exit = $LASTEXITCODE
if ($exit -ne 0) {
    $json | ForEach-Object { Write-Host $_ }
    throw "Input identity check failed."
}
$utf8 = New-Object System.Text.UTF8Encoding($false)
[IO.File]::WriteAllText($Output, (($json -join "`n") + "`n"), $utf8)
Write-Host "Input identity verified. Report: $Output"
