# Current user baseline (2026-10-28): OpenGL. Historical Vulkan helpers remain
# available for explicitly selected historical runs and the audio merge checks.
. (Join-Path $PSScriptRoot 'vulkan_policy.ps1')

function Get-RendererFromConfig([string]$Text) {
    $entries = [regex]::Matches($Text, '(?m)^backend-renderer:\s*([^\r\n#]+)')
    if ($entries.Count -ne 1) { throw 'config must contain one backend-renderer line' }
    $renderer = $entries[0].Groups[1].Value.Trim()
    if ($renderer -notin @('OpenGL','Vulkan')) { throw "Unknown renderer: $renderer" }
    return $renderer
}

function Assert-RendererBaseConfig([string]$RuntimeDirectory, [string]$Requested) {
    $base = Join-Path $RuntimeDirectory 'config.yml'
    if (Test-Path -LiteralPath $base) {
        $actual = Get-RendererFromConfig (Get-Content -LiteralPath $base -Raw)
        if ($actual -ne $Requested) { throw "Renderer merge conflict: base=$actual requested=$Requested" }
    }
}

function Assert-RendererBackendLog([string]$Log, [string]$Output, [datetime]$NotBefore, [string]$Expected) {
    $observed = @()
    $fresh = $false
    if (Test-Path -LiteralPath $Log) {
        $fresh = (Get-Item -LiteralPath $Log).LastWriteTimeUtc -ge $NotBefore.ToUniversalTime()
        # Initialization is at the beginning. Diagnostic GL logs can reach
        # gigabytes; never allocate the complete trace merely to identify it.
        $logText = (Get-Content -LiteralPath $Log -TotalCount 20000) -join "`n"
        if ($logText -match '\[renderer::gl::create\]') { $observed += 'OpenGL' }
        if ($logText -match '\[renderer::vulkan::VKState::create\]: Vulkan device:') { $observed += 'Vulkan' }
    }
    $actual = if ($fresh -and $observed.Count -eq 1) { $observed[0] } else { $null }
    $report = [ordered]@{expected=$Expected;actual=$actual;observed=@($observed);fresh=$fresh;
        verified=($actual -eq $Expected);scope='emulator-backend-identity-only'}
    [IO.File]::WriteAllText($Output, ($report | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
    if (-not $report.verified) { throw "$Expected initialization not verified; see $Output" }
}
