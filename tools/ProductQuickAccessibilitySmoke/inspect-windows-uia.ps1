#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)] [string]$Executable,
    [Parameter(Mandatory = $true)] [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

# Use the native UIA interface because .NET Framework cannot read FullDescription.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

[ComImport, Guid("30cbe57d-d9d0-452a-ab13-7ac5ac4825ee"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IOperatorAutomation {
    void CompareElements(); void CompareRuntimeIds(); void GetRootElement();
    IOperatorElement ElementFromHandle(IntPtr handle);
    void ElementFromPoint(); void GetFocusedElement(); void GetRootElementBuildCache();
    void ElementFromHandleBuildCache(); void ElementFromPointBuildCache(); void GetFocusedElementBuildCache();
    void CreateTreeWalker(); void ControlViewWalker(); void ContentViewWalker(); void RawViewWalker();
    void RawViewCondition(); void ControlViewCondition(); void ContentViewCondition(); void CreateCacheRequest();
    void CreateTrueCondition(); void CreateFalseCondition();
    [return: MarshalAs(UnmanagedType.Interface)]
    object CreatePropertyCondition(int property, [MarshalAs(UnmanagedType.Struct)] object value);
}
[ComImport, Guid("d22108aa-8ac5-49a5-837b-37bbb3d7591e"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IOperatorElement {
    void SetFocus(); void GetRuntimeId();
    IOperatorElement FindFirst(int scope, [MarshalAs(UnmanagedType.Interface)] object condition);
    void FindAll(); void FindFirstBuildCache(); void FindAllBuildCache(); void BuildUpdatedCache();
    [return: MarshalAs(UnmanagedType.Struct)]
    object GetCurrentPropertyValue(int property);
}
public static class OperatorNativeDescription {
    public static string Read(long handle, string name) {
        var automation = (IOperatorAutomation)Activator.CreateInstance(
            Type.GetTypeFromCLSID(new Guid("ff48dba4-60ef-4201-aa87-54103eef594e")));
        var condition = automation.CreatePropertyCondition(30005, name);
        var window = automation.ElementFromHandle(new IntPtr(handle));
        var element = window.FindFirst(4, condition);
        if (element == null) throw new InvalidOperationException("Native UIA element is absent: " + name);
        try { return (string)element.GetCurrentPropertyValue(30159); }
        finally {
            Marshal.ReleaseComObject(element); Marshal.ReleaseComObject(window);
            Marshal.ReleaseComObject(condition); Marshal.ReleaseComObject(automation);
        }
    }
}
"@
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $outputPath) {
    throw 'The probe output directory must be new so stale snapshots cannot pass inspection.'
}
$null = New-Item -ItemType Directory -Path $outputPath
$env:QT_QPA_PLATFORM = 'windows'
$env:QT_ACCESSIBILITY = '1'
Remove-Item Env:QT_QUICK_BACKEND -ErrorAction SilentlyContinue
$probeProcess = Start-Process -FilePath $executablePath -ArgumentList @('--operator-native-probe', ('"' + $outputPath + '"')) `
    -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $outputPath 'stdout.txt') `
    -RedirectStandardError (Join-Path $outputPath 'stderr.txt')
$null = $probeProcess.Handle
$observations = @()

try {
    for ($stage = 0; $stage -lt 6; $stage++) {
        $commandPath = Join-Path $outputPath 'stage.command'
        $temporaryCommand = Join-Path $outputPath 'stage.command.tmp'
        [System.IO.File]::WriteAllText($temporaryCommand, [string]$stage)
        Move-Item -LiteralPath $temporaryCommand -Destination $commandPath -Force
        $snapshotPath = Join-Path $outputPath ("stage-$stage.json")
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        while (-not (Test-Path -LiteralPath $snapshotPath)) {
            $probeProcess.Refresh()
            if ($probeProcess.HasExited -or [DateTime]::UtcNow -ge $deadline) {
                throw "The native probe did not publish stage $stage."
            }
            Start-Sleep -Milliseconds 100
        }
        $expected = Get-Content -LiteralPath $snapshotPath -Raw | ConvertFrom-Json
        $probeProcess.Refresh()
        $window = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]([long]$expected.window_handle))
        if (-not $window) { throw 'The product has no native UI Automation window.' }

        foreach ($node in $expected.nodes) {
            $condition = New-Object System.Windows.Automation.PropertyCondition(
                [System.Windows.Automation.AutomationElement]::NameProperty, [string]$node.name)
            $element = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
            if (-not $element) { throw "Native UI Automation cannot identify $($node.name) at stage $stage." }
            $actual = $element.Current
            $description = [OperatorNativeDescription]::Read([long]$expected.window_handle, [string]$node.name)
            if ($description -ne $node.description) {
                throw "Native description differs for $($node.name) at stage ${stage}: $($description)"
            }
            $focused = $false
            if ($node.PSObject.Properties.Name -contains 'focusable') {
                if (-not $actual.IsKeyboardFocusable) { throw "$($node.name) has no native keyboard focus target." }
                $element.SetFocus()
                $focused = $element.Current.HasKeyboardFocus
                if (-not $focused) { throw "Native focus did not reach $($node.name)." }
            } else {
                if ($actual.IsEnabled -ne $node.enabled) { throw "Native availability differs for $($node.name)." }
                if (-not $actual.IsEnabled -and [string]::IsNullOrWhiteSpace($description)) {
                    throw "Disabled $($node.name) has no accessible reason."
                }
            }
            $observations += [pscustomobject]@{
                stage = $stage
                name = $actual.Name
                description = $description
                control_type = $actual.ControlType.ProgrammaticName
                enabled = $actual.IsEnabled
                focusable = $actual.IsKeyboardFocusable
                focused = $focused
                provider = $actual.FrameworkId
                process_id = $actual.ProcessId
            }
        }
    }
    [System.IO.File]::WriteAllText((Join-Path $outputPath 'stage.command'), '6')
    if (-not $probeProcess.WaitForExit(10000) -or $probeProcess.ExitCode -ne 0) {
        throw "The native accessibility probe did not finish successfully (exit $($probeProcess.ExitCode))."
    }
    $report = [pscustomobject]@{
        status = 'pass'
        platform = 'Windows UI Automation'
        executable_sha256 = (Get-FileHash -LiteralPath $executablePath -Algorithm SHA256).Hash
        fixture_sha256 = (Get-FileHash -LiteralPath (Join-Path $outputPath 'operator-fixture.pdf') -Algorithm SHA256).Hash
        observations = $observations
    }
    $report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $outputPath 'native-uia.json') -Encoding UTF8
    Write-Output "Native UI Automation passed: $($observations.Count) observations across six operator states."
} finally {
    $probeProcess.Refresh()
    if (-not $probeProcess.HasExited) { Stop-Process -Id $probeProcess.Id }
}
