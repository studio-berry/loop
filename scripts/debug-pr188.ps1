# PR #188 local debug harness — writes NDJSON to debug-b0e75b.log
param(
    [string]$LogPath = "debug-b0e75b.log",
    [string]$PdfTool = "build\usr\bin\Release\PdfTool.exe",
    [string]$QtBin = "C:\.dev\repos\frisket\qt\6.11.1\msvc2022_64\bin"
)

$ErrorActionPreference = "Continue"
$sessionId = "b0e75b"
$runId = "pr188-local"
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

if ($QtBin -and (Test-Path $QtBin)) {
    $env:PATH = "$QtBin;$repoRoot\build\usr\bin\Release;$env:PATH"
}
$env:QT_QPA_PLATFORM = "offscreen"

function Write-DebugLog {
    param(
        [string]$HypothesisId,
        [string]$Location,
        [string]$Message,
        [hashtable]$Data
    )
    $entry = @{
        sessionId = $sessionId
        runId = $runId
        hypothesisId = $HypothesisId
        location = $Location
        message = $Message
        data = $Data
        timestamp = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
    } | ConvertTo-Json -Compress -Depth 6
    Add-Content -Path $LogPath -Value $entry -Encoding utf8
}

function Invoke-PdfToolJson {
    param([string[]]$Args)
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = (Resolve-Path $PdfTool).Path
    $psi.Arguments = ($Args -join " ")
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $p = [System.Diagnostics.Process]::Start($psi)
    $stdout = $p.StandardOutput.ReadToEnd()
    $stderr = $p.StandardError.ReadToEnd()
    $p.WaitForExit()
  return [pscustomobject]@{
        ExitCode = $p.ExitCode
        Stdout = $stdout.Trim()
        Stderr = $stderr.Trim()
        Json = try { $stdout.Trim() | ConvertFrom-Json } catch { $null }
    }
}

Write-DebugLog "H0" "debug-pr188.ps1:start" "debug harness started" @{ pdfTool = $PdfTool; exists = (Test-Path $PdfTool) }

if (-not (Test-Path $PdfTool)) {
    Write-DebugLog "H0" "debug-pr188.ps1:missing-binary" "PdfTool not found" @{ path = $PdfTool }
    exit 2
}

# H1: JSON envelope contract
$help = Invoke-PdfToolJson @("help", "--console-format", "json")
Write-DebugLog "H1" "PdfTool help json" "envelope smoke" @{
    exitCode = $help.ExitCode
    stderrEmpty = [string]::IsNullOrEmpty($help.Stderr)
    schemaVersion = $help.Json.schema_version
    envelopeExit = $help.Json.exit_code
    exitMatches = ($help.ExitCode -eq $help.Json.exit_code)
}

# H2: OCR partial failure exit code vs plugin whitelist (0/1 only)
$mockSidecar = "loupe-ocr\tools\mock_ocr_sidecar.py"
$fixture = "loupe-preflight\testdata\fixtures\image-dpi-low.pdf"
if ((Test-Path $mockSidecar) -and (Test-Path $fixture)) {
    $env:LOUPE_OCR_MOCK_MODE = "malformed-json"
    $ocr = Invoke-PdfToolJson @("ocr", $fixture, "--console-format", "json", "--sidecar", $mockSidecar)
    Remove-Item Env:LOUPE_OCR_MOCK_MODE -ErrorAction SilentlyContinue
    Write-DebugLog "H2" "PdfTool ocr malformed-json" "OCR partial failure contract" @{
        processExit = $ocr.ExitCode
        envelopeExit = $ocr.Json.exit_code
        status = $ocr.Json.status
        pluginWouldAccept = ($ocr.ExitCode -eq 0 -or $ocr.ExitCode -eq 1)
        testExpectsExit1 = $true
    }
}

# H3: Repair commit gate requires profile
$bleedFixture = "loupe-preflight\testdata\fixtures\bleed-missing.pdf"
if (Test-Path $bleedFixture) {
    $repairNoProfile = Invoke-PdfToolJson @(
        "repair", $bleedFixture,
        "--operation", "add-bleed", "--bleed_mm=3",
        "--overwrite", "debug-pr188-out.pdf",
        "--console-format", "json"
    )
    Write-DebugLog "H3" "PdfTool repair no profile" "repair postflight gate" @{
        processExit = $repairNoProfile.ExitCode
        envelopeExit = $repairNoProfile.exit_code
        status = $repairNoProfile.Json.status
        diagnostics = @($repairNoProfile.Json.diagnostics | ForEach-Object { $_.code })
    }
    Remove-Item "debug-pr188-out.pdf" -ErrorAction SilentlyContinue
}

# H4: Capabilities discovery deterministic
$capA = Invoke-PdfToolJson @("capabilities", "--console-format", "json")
$capB = Invoke-PdfToolJson @("capabilities", "--console-format", "json")
Write-DebugLog "H4" "PdfTool capabilities" "deterministic discovery" @{
    exitA = $capA.ExitCode
    exitB = $capB.ExitCode
    identicalStdout = ($capA.Stdout -eq $capB.Stdout)
    commandCount = @($capA.Json.data.commands).Count
}

# H5: Preflight nested report boundary
if (Test-Path $bleedFixture) {
    $profile = "loupe-preflight\profiles\loupe-default.json"
    if (Test-Path $profile) {
        $pf = Invoke-PdfToolJson @("preflight", $bleedFixture, "--profile", $profile)
        Write-DebugLog "H5" "PdfTool preflight" "nested report schema" @{
            processExit = $pf.ExitCode
            reportSchema = $pf.Json.data.report.schema_version
            pass = $pf.Json.data.report.pass
            inspectionComplete = $pf.Json.data.report.inspection_complete
        }
    }
}

Write-DebugLog "H0" "debug-pr188.ps1:done" "debug harness finished" @{}
