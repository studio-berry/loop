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

if ($GhRepo -and $AssetId) {
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest `
        -Uri "https://api.github.com/repos/$GhRepo/releases/assets/$AssetId" `
        -Headers @{ 'Accept' = 'application/octet-stream'; 'X-GitHub-Api-Version' = '2022-11-28' } `
        -OutFile $OutFile
}
elseif ($Url) {
    $ProgressPreference = 'SilentlyContinue'
    Invoke-WebRequest -Uri $Url -OutFile $OutFile
}
else {
    throw "download_verified.ps1: provide either -Url or (-GhRepo and -AssetId)."
}

$actualHash = (Get-FileHash $OutFile -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actualHash -ne $Sha256.ToLowerInvariant()) {
    throw "download_verified.ps1: SHA256 mismatch for '$OutFile': expected $Sha256, got $actualHash"
}

Write-Host "download_verified.ps1: verified $OutFile ($actualHash)"