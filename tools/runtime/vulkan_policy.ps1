# User policy (2026-10-27): all new emulator runs use Vulkan.
function Assert-VulkanConfig([string]$Text) {
    $entries = [regex]::Matches($Text, '(?m)^backend-renderer:\s*([^\r\n#]+)')
    if ($entries.Count -ne 1 -or $entries[0].Groups[1].Value.Trim() -ne 'Vulkan') {
        throw 'Vulkan required: config must contain exactly one backend-renderer: Vulkan'
    }
}

function Assert-VulkanBaseConfig([string]$RuntimeDirectory) {
    $base = Join-Path $RuntimeDirectory 'config.yml'
    # An absent base uses Vita3K defaults (Vulkan). An existing base must agree:
    # the config merge can otherwise silently retain OpenGL.
    if (Test-Path -LiteralPath $base) {
        Assert-VulkanConfig (Get-Content -LiteralPath $base -Raw)
    }
}

function Get-AudioBackendFromConfig([string]$Text) {
    $entries = [regex]::Matches($Text, '(?m)^audio-backend:\s*([^\r\n#]+)')
    if ($entries.Count -ne 1) {
        throw 'config must contain exactly one audio-backend line'
    }
    return $entries[0].Groups[1].Value.Trim()
}

# B48: the config merge ignores a requested value equal to the built-in
# default (SDL) when the executable-directory base config holds another value.
# The effective adapter is therefore only unambiguous when both agree. An
# absent base means defaults apply, so a requested non-default still wins.
function Assert-AudioBackendBaseConfig([string]$RuntimeDirectory, [string]$Requested) {
    if (-not $Requested) { throw 'requested audio backend is missing' }
    $base = Join-Path $RuntimeDirectory 'config.yml'
    if (Test-Path -LiteralPath $base) {
        $baseBackend = Get-AudioBackendFromConfig (Get-Content -LiteralPath $base -Raw)
        if ($baseBackend -ne $Requested) {
            throw "Audio backend merge conflict: base config '$baseBackend' differs from requested '$Requested'"
        }
        return $baseBackend
    }
    return $null
}

function Assert-VulkanBackendLog([string]$Log, [string]$Output, [datetime]$NotBefore) {
    $observed = @()
    $hash = $null
    $fresh = $false
    if (Test-Path -LiteralPath $Log) {
        $fresh = (Get-Item -LiteralPath $Log).LastWriteTimeUtc -ge $NotBefore.ToUniversalTime()
        $text = Get-Content -LiteralPath $Log -Raw
        $hash = (Get-FileHash -LiteralPath $Log -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($text -match '\[renderer::gl::create\]') { $observed += 'OpenGL' }
        if ($text -match '\[renderer::vulkan::VKState::create\]: Vulkan device:') { $observed += 'Vulkan' }
    }
    $actual = if ($fresh -and $observed.Count -eq 1) { $observed[0] } else { $null }
    $report = [ordered]@{expected='Vulkan';actual=$actual;observed=@($observed);
        fresh=$fresh;verified=($actual -eq 'Vulkan');log_sha256=$hash;
        scope='emulator-backend-identity-only'}
    [IO.File]::WriteAllText($Output, ($report | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))
    if (-not $report.verified) {
        throw "Vulkan initialization not verified; see $Output"
    }
}
