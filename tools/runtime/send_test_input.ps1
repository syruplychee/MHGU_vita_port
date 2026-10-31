# Controller-only input for an explicitly enabled M6 test session. No window
# focus, synthetic keyboard events, save edits, or game-state changes.
param(
    [ValidateSet('Neutral','A','B','X','Y','Start','Select','Up','Down','Left','Right','L','R')]
    [string]$Button='Neutral',
    [ValidateRange(-32767,32767)][int]$LeftX=0,
    [ValidateRange(-32767,32767)][int]$LeftY=0,
    [ValidateRange(0,30000)][int]$DurationMs=4000,
    [string]$DataDir=''
)
$ErrorActionPreference='Stop'
if (-not $DataDir) { $DataDir=Join-Path $PSScriptRoot '../../build/vita3k-pref/ux0/data/mhgu-vita-port' }
if (-not (Test-Path -LiteralPath (Join-Path $DataDir 'input-live.enable'))) {
    throw 'Live test input is not enabled for this session'
}
$mask=@{Neutral=0;A=8192;B=4096;X=32768;Y=16384;Start=16;Select=32;Up=1;Down=2;Left=4;Right=8;L=256;R=512}[$Button]
$sequence=([DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() % 4294967295)
if ($sequence -eq 0) { $sequence=1 }
$line="$sequence $mask $LeftX $LeftY $DurationMs"
$temporary=Join-Path $DataDir 'input-live.tmp'
[IO.File]::WriteAllText($temporary, "$line`n", (New-Object Text.UTF8Encoding($false)))
Move-Item -LiteralPath $temporary -Destination (Join-Path $DataDir 'input-live.txt') -Force
Write-Output "TEST_INPUT seq=$sequence button=$Button l=($LeftX,$LeftY) duration_ms=$DurationMs"
