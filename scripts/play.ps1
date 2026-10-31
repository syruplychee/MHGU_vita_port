param(
    [string]$Pref = "",
    [switch]$RestoreAfter,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RunnerArgs = @()
)
$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Pref) { $Pref = Join-Path $Root "build/vita3k-pref" }

# Always preserve the current save before handing control to the game. The
# backup is written below local/ and never enters Git. A first launch may not
# have a savedata directory yet; that is not an error.
$savedata = Join-Path $Pref "ux0/data/mhgu-vita-port/savedata"
$backupPath = ""
if (Test-Path -LiteralPath $savedata -PathType Container) {
    $backupOutput = & (Join-Path $PSScriptRoot "backup-saves.ps1") -Pref $Pref
    $backupOutput | ForEach-Object { Write-Host $_ }
    $backupLine = $backupOutput | Where-Object { $_ -match "Savedata backed up to: (.+)$" } | Select-Object -Last 1
    if ($backupLine -and $backupLine -match "Savedata backed up to: (.+)$") { $backupPath = $Matches[1].Trim() }
} else {
    Write-Host "No existing savedata; skipping pre-run backup."
}

$exit = 0
try {
    & (Join-Path $Root "tools/runtime/run_gui_play.ps1") @RunnerArgs
    $exit = $LASTEXITCODE
} finally {
    if ($RestoreAfter -and $backupPath) {
        Write-Host "Restoring savedata from: $backupPath"
        & (Join-Path $PSScriptRoot "backup-saves.ps1") -Pref $Pref -Restore $backupPath -Replace
    } elseif ($backupPath) {
        Write-Host "Saves left in place. Backup: $backupPath"
    }
}
exit $exit
