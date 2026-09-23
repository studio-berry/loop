# Shared Qt runtime discovery for the Windows development scripts.
#
# Loop's Qt runtime comes from LOOP_QT_ROOT (an aqt install), not from vcpkg, so
# no Qt DLL is ever copied beside a build-tree executable. A script that starts
# one of those executables must therefore put LOOP_QT_ROOT\bin on PATH first;
# otherwise Windows blocks in the loader with "Qt6Core.dll was not found" (and
# "Qt6Quick.dll was not found" for the editor/Quick binaries).
#
# LOOP_QT_ROOT wins over QT_ROOT_DIR because that is the variable the CMake cache
# uses. When neither is set, Qt is assumed to be on PATH already (CI installs Qt
# that way) and this is a no-op.
function Add-LoopQtRuntimeToPath {
    [CmdletBinding()]
    [OutputType([string])]
    param()

    if ([System.Environment]::OSVersion.Platform -ne [System.PlatformID]::Win32NT) {
        Write-Verbose "Non-Windows Qt libraries use the platform loader path; no DLL PATH adjustment is needed."
        return $null
    }

    $qtRoot = if ($env:LOOP_QT_ROOT) { $env:LOOP_QT_ROOT } elseif ($env:QT_ROOT_DIR) { $env:QT_ROOT_DIR } else { "" }
    if (-not $qtRoot) {
        Write-Verbose "Neither LOOP_QT_ROOT nor QT_ROOT_DIR is set; assuming Qt is already on PATH."
        return $null
    }

    $binDir = Join-Path $qtRoot "bin"
    foreach ($dll in @("Qt6Core.dll", "Qt6Quick.dll")) {
        if (-not (Test-Path -LiteralPath (Join-Path $binDir $dll))) {
            throw "LOOP_QT_ROOT ($qtRoot) has no bin\$dll. Point LOOP_QT_ROOT at the Qt $($env:LOOP_QT_VERSION) msvc2022_64 aqt install; the Qt online-installer kit at C:\Qt\<version>\msvc2022_64 ships no qtbase DLLs."
        }
    }

    if (($env:PATH -split ";") -notcontains $binDir) {
        $env:PATH = "$binDir;$env:PATH"
    }
    return $binDir
}
