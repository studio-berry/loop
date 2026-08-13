# Verified-download helper for the Windows packaging steps.
#
# Downloads an external packaging tool and asserts its SHA-256 before the
# binary is used. Two mutually exclusive modes:
#
#   -GhRepo <owner/repo> -AssetId <asset-id> : GitHub release asset (immutable id)
#   -Url <url>                               : versioned release archive
#
# Both require -OutFile <path> and -Sha256 <expected sha256>.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [string]$Url,

    [Parameter(Mandatory = $false)]
    [string]$GhRepo,

    [Parameter(Mandatory = $false)]
    [string]$AssetId,

    [Parameter(Mandatory = $true)]
    [string]$OutFile,

    [Parameter(Mandatory = $true)]
    [string]$Sha256
)

$ErrorActionPreference = 'Stop'

if ($Sha256 -notmatch '^[0-9A-Fa-f]{64}$') {
    throw "download_verified.ps1: -Sha256 must be 64 hexadecimal characters."
}

if ($GhRepo -and $AssetId) {
    if ($GhRepo -notmatch '^[^/\s]+/[^/\s]+$') {
        throw "download_verified.ps1: -GhRepo must be in owner/repository form."
    }
    if ($AssetId -notmatch '^[1-9][0-9]*$') {
        throw "download_verified.ps1: -AssetId must be a positive integer."
    }
}
elseif ($Url) {
    if ($Url -notmatch '^https://') {
        throw "download_verified.ps1: -Url must use HTTPS."
    }
    $normalizedUrl = $Url.ToLowerInvariant()
    if ($normalizedUrl.Contains('/releases/download/continuous') -or
        $normalizedUrl.Contains('/releases/download/latest') -or
        $normalizedUrl.Contains('/releases/latest')) {
        throw "download_verified.ps1: mutable release URL is forbidden: $Url"
    }
}

$temporaryOutFile = "$OutFile.$([System.IO.Path]::GetRandomFileName()).download"

try {
    if ($GhRepo -and $AssetId) {
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest `
            -Uri "https://api.github.com/repos/$GhRepo/releases/assets/$AssetId" `
            -Headers @{ 'Accept' = 'application/octet-stream'; 'X-GitHub-Api-Version' = '2022-11-28' } `
            -OutFile $temporaryOutFile
    }
    elseif ($Url) {
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Url -OutFile $temporaryOutFile
    }
    else {
        throw "download_verified.ps1: provide either -Url or (-GhRepo and -AssetId)."
    }

    $actualHash = (Get-FileHash $temporaryOutFile -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $Sha256.ToLowerInvariant()) {
        throw "download_verified.ps1: SHA256 mismatch for '$OutFile': expected $Sha256, got $actualHash"
    }

    Move-Item -LiteralPath $temporaryOutFile -Destination $OutFile -Force
}
finally {
    Remove-Item -LiteralPath $temporaryOutFile -Force -ErrorAction SilentlyContinue
}

Write-Host "download_verified.ps1: verified $OutFile ($actualHash)"
