$ErrorActionPreference='Stop'
$path=Join-Path $PSScriptRoot '../tools/runtime/run_gui_play.ps1'
$tokens=$null;$errors=$null
$ast=[System.Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw 'Runner parse failed' }
$function=$ast.Find({param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Select-VPMainWindow'},$true)
if (-not $function) { throw 'Production selector missing' }
. ([scriptblock]::Create($function.Extent.Text))
$game='11930984|vis=True|rect=671,312,1889,1039|client=1200x680|title=MHGU Vita Port | OpenGL'
$launcher='28707322|vis=True|rect=-32000,-32000,-31801,-31966|client=0x0|title=Vita3K v0.2.1 4106-d7edcbe9'
$result=Select-VPMainWindow -Windows @($game,$launcher) -Fallback ([IntPtr]11930984)
if ($result.Handle.ToInt64() -ne 28707322) { throw 'Minimized owned launcher was not selected' }
$empty='12345|vis=True|rect=0,0,0,0|client=0x0|title=Default IME'
$result=Select-VPMainWindow -Windows @($empty) -Fallback ([IntPtr]11930984)
if ($result.Handle.ToInt64() -ne 11930984) { throw 'Unrelated zero-size window selected' }
Write-Output 'Runtime main-window selection passed'
