[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Installer,
    [Parameter(Mandatory = $true)][string]$PortableExecutable,
    [Parameter(Mandatory = $true)][string]$Version,
    [Parameter(Mandatory = $true)][string]$SourceRoot,
    [string]$BoardValidator,
    [switch]$SkipApplicationLaunch,
    [switch]$DetectApplicationLaunchSupport,
    [string]$PreviousInstaller,
    [string]$PreviousVersion,
    [string]$PythonExecutable = "python"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class SawerInstallerWindows {
    private delegate bool EnumProc(IntPtr window, IntPtr state);
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumProc callback, IntPtr state);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetWindowText(IntPtr window, StringBuilder title, int size);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window, uint message, IntPtr wparam, IntPtr lparam);
    public static IntPtr Find(int pid, string expected) {
        IntPtr found = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr state) {
            uint owner; GetWindowThreadProcessId(window, out owner);
            if (owner != pid) return true;
            var title = new StringBuilder(1024);
            GetWindowText(window, title, title.Capacity);
            if (title.ToString().Contains(expected)) { found = window; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
"@

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function Invoke-CheckedProcess([string]$FilePath, [string[]]$ArgumentList, [int]$ExpectedExitCode = 0) {
    $process = Start-Process -FilePath $FilePath -ArgumentList $ArgumentList -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(60000)) {
        $process.Kill()
        throw "$FilePath did not exit within 60 seconds"
    }
    Assert-Condition ($process.ExitCode -eq $ExpectedExitCode) "$FilePath exited with $($process.ExitCode), expected $ExpectedExitCode"
}
function Invoke-CheckedCommand([string]$FilePath, [string[]]$ArgumentList) {
    & $FilePath @ArgumentList
    Assert-Condition ($LASTEXITCODE -eq 0) "$FilePath exited with $LASTEXITCODE"
}
function Get-DefaultRegistryValue([string]$Subkey) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($Subkey)
    if ($null -eq $key) { return $null }
    try { return $key.GetValue("") } finally { $key.Dispose() }
}
function Get-DesktopDirectory {
    $desktop = [Environment]::GetFolderPath([Environment+SpecialFolder]::DesktopDirectory)
    if ([string]::IsNullOrWhiteSpace($desktop)) {
        $desktop = [Environment]::ExpandEnvironmentVariables((Get-ItemPropertyValue "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders" "Desktop"))
    }
    return $desktop
}
function Get-PreferenceHashes {
    $hashes = @{}
    foreach ($entry in Get-ChildItem -LiteralPath $preferencesRoot) {
        # The running app owns its log and disposable preview cache, not user preferences.
        if ($entry.PSIsContainer -and $entry.Name -eq 'previews') { continue }
        if (-not $entry.PSIsContainer -and $entry.Name -eq 'Sawer.log') { continue }
        $files = if ($entry.PSIsContainer) {
            Get-ChildItem -LiteralPath $entry.FullName -File -Recurse
        } else { $entry }
        foreach ($file in $files) {
            $hashes[$file.FullName] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
        }
    }
    return $hashes
}
function Assert-Preserved([hashtable]$Preferences) {
    Assert-Condition ((Get-FileHash -LiteralPath $boardPath -Algorithm SHA256).Hash -eq $boardHash) "Installer operation changed or removed the board"
    foreach ($file in $Preferences.Keys) {
        Assert-Condition (Test-Path -LiteralPath $file -PathType Leaf) "Preferences were removed: $file"
        Assert-Condition ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -eq $Preferences[$file]) "Preferences changed: $file"
    }
}
$ownedProcesses = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
function Start-TestApplication([string]$LaunchPath, [string]$ExpectedTitle) {
    $process = Start-Process -FilePath $LaunchPath -WindowStyle Hidden -PassThru
    Assert-Condition ($null -ne $process) "The shell did not return the launched application process"
    $ownedProcesses.Add($process)
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $process.Refresh()
        Assert-Condition (-not $process.HasExited) "The application exited before opening $ExpectedTitle"
        $window = [SawerInstallerWindows]::Find($process.Id, $ExpectedTitle)
        if ($window -ne [IntPtr]::Zero) {
            Assert-Condition ($process.Path -eq $installedExecutable) "The shell launched a different executable"
            return $process
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "The application did not display the expected board title: $ExpectedTitle"
}
function Close-TestApplication([System.Diagnostics.Process]$Process) {
    if ($Process.HasExited) { return }
    $window = [SawerInstallerWindows]::Find($Process.Id, "Sawer")
    Assert-Condition ($window -ne [IntPtr]::Zero) "The test application window disappeared"
    [void][SawerInstallerWindows]::PostMessage($window, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
    if (-not $Process.WaitForExit(10000)) {
        $Process.Kill()
        throw "Sawer did not close normally"
    }
    Assert-Condition ($Process.ExitCode -eq 0) "Sawer failed during shutdown"
}
function Assert-Shortcut([string]$Shortcut, [string]$Executable) {
    Assert-Condition (Test-Path -LiteralPath $Shortcut -PathType Leaf) "Shortcut is missing: $Shortcut"
    # WScript.Shell reads ANSI targets and corrupts paths outside the system code page.
    $shell = New-Object -ComObject Shell.Application
    $fullPath = [IO.Path]::GetFullPath($Shortcut)
    $folder = $shell.Namespace([IO.Path]::GetDirectoryName($fullPath))
    Assert-Condition ($null -ne $folder) "Shortcut directory could not be read: $Shortcut"
    $item = $folder.ParseName([IO.Path]::GetFileName($fullPath))
    Assert-Condition ($null -ne $item) "Shortcut could not be read: $Shortcut"
    $link = $item.GetLink
    $target = $link.Path
    Assert-Condition (-not [string]::IsNullOrEmpty($target)) "Shortcut target is empty: $Shortcut"
    Assert-Condition ([string]::Equals(
        [IO.Path]::GetFullPath($target), [IO.Path]::GetFullPath($Executable),
        [StringComparison]::OrdinalIgnoreCase)) "Shortcut targets a different application: $Shortcut; actual '$target', expected '$Executable'"
    Assert-Condition ([string]::IsNullOrEmpty($link.Arguments)) "Shortcut contains unexpected arguments: $Shortcut"
}
function Assert-Installed {
    Assert-Condition ((Get-FileHash -LiteralPath $installedExecutable -Algorithm SHA256).Hash -eq $portableHash) "Installed executable differs from the portable asset"
    Assert-Condition ((Get-Item -LiteralPath $installedExecutable).VersionInfo.ProductVersion -eq $Version) "Installed application version is wrong"
    foreach ($shortcut in @($desktopShortcut, $startMenuShortcut)) {
        Assert-Shortcut $shortcut $installedExecutable
    }
    $expected = '"' + $installedExecutable + '" "%1"'
    Assert-Condition ((Get-DefaultRegistryValue "Software\Classes\Sawer.Board\shell\open\command") -eq $expected) "The association command is not quoted correctly"
    Assert-Condition ((Get-ItemPropertyValue "HKCU:\Software\RegisteredApplications" "Sawer") -eq "Software\Sawer\Capabilities") "Default Apps registration is missing"
    Assert-Condition ((Get-ItemPropertyValue $uninstallKey "DisplayVersion") -eq $Version) "Apps registration has an incorrect version"
}
function Uninstall-TestApplication {
    Invoke-CheckedProcess $uninstaller @("/S")
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ((Test-Path -LiteralPath $installRoot) -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 100 }
    Assert-Condition (-not (Test-Path -LiteralPath $installRoot)) "Uninstall left application files"
    foreach ($path in @($desktopShortcut, $startMenuShortcut, $uninstallKey, "HKCU:\Software\Classes\Sawer.Board", "HKCU:\Software\Classes\Applications\Sawer.exe", "HKCU:\Software\Sawer\Capabilities")) {
        Assert-Condition (-not (Test-Path -LiteralPath $path)) "Uninstall left registration or a shortcut: $path"
    }
    $registered = Get-ItemProperty -LiteralPath "HKCU:\Software\RegisteredApplications" -Name Sawer -ErrorAction SilentlyContinue
    Assert-Condition ($null -eq $registered) "Uninstall left Default Apps registration"
    $openWith = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey("Software\Classes\.sawer\OpenWithProgids")
    if ($null -ne $openWith) {
        try { Assert-Condition (-not ($openWith.GetValueNames() -contains "Sawer.Board")) "Uninstall left an OpenWith entry" }
        finally { $openWith.Dispose() }
    }
}

$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$portablePath = (Resolve-Path -LiteralPath $PortableExecutable).Path
$sourcePath = (Resolve-Path -LiteralPath $SourceRoot).Path
if (-not $BoardValidator) { $BoardValidator = Join-Path $sourcePath "build\sawer-format.exe" }
$validatorPath = (Resolve-Path -LiteralPath $BoardValidator).Path
if ($PreviousInstaller) {
    $previousPath = (Resolve-Path -LiteralPath $PreviousInstaller).Path
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($PreviousVersion)) "PreviousVersion is required"
    Assert-Condition ([version]$PreviousVersion -lt [version]$Version) "PreviousVersion must be older"
    Assert-Condition ((Get-Item -LiteralPath $previousPath).VersionInfo.ProductVersion -eq $PreviousVersion) "Previous installer version mismatch"
}
$temporaryRoot = [IO.Path]::GetFullPath($(if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }))
$testRoot = Join-Path $temporaryRoot "Sawer installer test $([char]0x753b)$([char]0x677f) $([Guid]::NewGuid())"
$installRoot = Join-Path $testRoot "Installed Sawer"
$boardPath = Join-Path $testRoot "$([char]0x8ba1)$([char]0x5212) preserved board.sawer"
$installedExecutable = Join-Path $installRoot "Sawer.exe"
$uninstaller = Join-Path $installRoot "Uninstall.exe"
$desktopShortcut = Join-Path (Get-DesktopDirectory) "Sawer.lnk"
$startMenuShortcut = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::Programs)) "Sawer.lnk"
$preferencesRoot = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)) "Sawer\Sawer"
$uninstallKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{49266FD7-A24C-40B7-9364-AB6615BF4510}"
foreach ($path in @($desktopShortcut, $startMenuShortcut, $uninstallKey, $preferencesRoot, "HKCU:\Software\Classes\Sawer.Board", "HKCU:\Software\Classes\.sawer", "HKCU:\Software\Classes\Applications\Sawer.exe", "HKCU:\Software\Sawer", "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\FileExts\.sawer\UserChoice")) {
    Assert-Condition (-not (Test-Path -LiteralPath $path)) "Use a disposable account; existing Sawer state: $path"
}
Assert-Condition ($null -eq (Get-Process -Name Sawer -ErrorAction SilentlyContinue)) "Installer tests require no running Sawer processes"
Assert-Condition ($null -eq (Get-ItemProperty "HKCU:\Software\RegisteredApplications" -Name Sawer -ErrorAction SilentlyContinue)) "Sawer is already registered"

$otherHandlerCreated = $false
try {
New-Item -ItemType Directory -Path $testRoot | Out-Null
New-Item -ItemType Directory -Path $preferencesRoot -Force | Out-Null
$encoding = [Text.UTF8Encoding]::new($false)
$header = '{"format":"sawer","version":1,"board_id":"00000000-0000-0000-0000-000000000001","bounds":[-1000000,-1000000,1000000,1000000]}'
$operation = '{"seq":1,"op":"put","id":"00000000-0000-0000-0000-000000000002","object":{"type":"line","start":[0,0],"end":[100,100],"z":0,"style":{"stroke":[0,0,0,255],"fill":null,"width":2}}}'
$newline = [Environment]::NewLine
[IO.File]::WriteAllText($boardPath, "$header$newline$operation$newline", $encoding)
$settingsPath = Join-Path $preferencesRoot "recent-files.json"
[IO.File]::WriteAllText($settingsPath, (@{version=1;recent=@($boardPath);zoom=@();drawing=@{light_theme=$true}} | ConvertTo-Json -Depth 6), $encoding)
[IO.File]::WriteAllText((Join-Path $preferencesRoot "preserve-me.txt"), "User preferences sentinel", $encoding)
$otherHandlerCreated = $false
    Invoke-CheckedCommand $validatorPath @("compact", $boardPath)
    Invoke-CheckedCommand $validatorPath @("validate", $boardPath)
    $boardHash = (Get-FileHash -LiteralPath $boardPath -Algorithm SHA256).Hash
    $portableHash = (Get-FileHash -LiteralPath $portablePath -Algorithm SHA256).Hash
    $metadata = (Get-Item -LiteralPath $installerPath).VersionInfo
    Assert-Condition ($metadata.ProductVersion -eq $Version) "Installer version mismatch"
    Assert-Condition ($metadata.FileDescription -eq "Sawer Installer") "Installer resource metadata is missing"
    $icon = [Drawing.Icon]::ExtractAssociatedIcon($installerPath)
    Assert-Condition ($null -ne $icon) "Installer icon is missing"
    $icon.Dispose()
    Invoke-CheckedCommand $PythonExecutable @((Join-Path $sourcePath "tools\check_release.py"), "--source-root", $sourcePath, "--executable", $portablePath)
    if ($DetectApplicationLaunchSupport) {
        # Probe only after the clean-account guard and test preferences exist.
        & $portablePath --gpu-info
        $SkipApplicationLaunch = ($LASTEXITCODE -ne 0)
    }
    $before = Get-PreferenceHashes
    if ($PreviousInstaller) {
        Invoke-CheckedProcess $previousPath @("/S", "/D=$installRoot")
        Assert-Condition ((Get-ItemPropertyValue $uninstallKey "DisplayVersion") -eq $PreviousVersion) "Previous installer did not register the older version"
        $previousHash = (Get-FileHash -LiteralPath $installedExecutable -Algorithm SHA256).Hash
        Assert-Preserved $before
    } else {
        Write-Output "SKIP older-version upgrade: no previous published installer supplied"
    }
    Invoke-CheckedProcess $installerPath @("/S", "/D=$installRoot")
    Assert-Installed
    Assert-Preserved $before
    if ($PreviousInstaller) { Assert-Condition ($previousHash -ne $portableHash) "Upgrade did not exercise a changed application payload" }
    Assert-Condition ((Get-DefaultRegistryValue "Software\Classes\.sawer") -eq "Sawer.Board") "Clean install did not register the board handler"
    $lock = $null
    $application = $null
    if ($SkipApplicationLaunch) {
        Write-Output "SKIP GUI launches: the runner cannot initialize SDL GPU"
        $lock = [IO.File]::Open($installedExecutable, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    } else {
        foreach ($shortcut in @($desktopShortcut, $startMenuShortcut)) {
            $application = Start-TestApplication $shortcut "Untitled"
            Close-TestApplication $application
        }
        $application = Start-TestApplication $boardPath ([IO.Path]::GetFileNameWithoutExtension($boardPath))
    }
    try {
        $before = Get-PreferenceHashes
        Invoke-CheckedProcess $installerPath @("/S", "/D=$installRoot") 2
        Assert-Installed
        Assert-Preserved $before
        # Avoid NSIS's temporary relaunch so the blocked exit code is observed.
        Invoke-CheckedProcess $uninstaller @("/S", "_?=$installRoot") 2
        Assert-Installed
        Assert-Preserved $before
    } finally {
        if ($null -ne $lock) { $lock.Dispose() }
        if ($null -ne $application -and -not $application.HasExited) { Close-TestApplication $application }
    }
    Invoke-CheckedCommand $validatorPath @("validate", $boardPath)
    $before = Get-PreferenceHashes
    Invoke-CheckedProcess $installerPath @("/S", "/D=$installRoot")
    Assert-Installed
    Assert-Preserved $before
    Uninstall-TestApplication
    Assert-Preserved $before
    Assert-Condition ($null -eq (Get-DefaultRegistryValue "Software\Classes\.sawer")) "Uninstall left its default association"

    $otherHandlerCreated = $true
    New-Item "HKCU:\Software\Classes\.sawer" -Force | Out-Null
    Set-Item "HKCU:\Software\Classes\.sawer" -Value "SawerInstallerTest.Other"
    Invoke-CheckedProcess $installerPath @("/S", "/D=$installRoot")
    Assert-Installed
    Assert-Condition ((Get-DefaultRegistryValue "Software\Classes\.sawer") -eq "SawerInstallerTest.Other") "Install replaced another default handler"
    Uninstall-TestApplication
    Assert-Preserved $before
    Assert-Condition ((Get-DefaultRegistryValue "Software\Classes\.sawer") -eq "SawerInstallerTest.Other") "Uninstall removed another default handler"
    Write-Output "Installer, association, running-app protection, reinstall and preservation checks passed"
} finally {
    foreach ($process in $ownedProcesses) {
        if (-not $process.HasExited) {
            try { Close-TestApplication $process } catch { $process.Kill(); $process.WaitForExit() }
        }
    }
    if (Test-Path -LiteralPath $uninstaller) {
        try {
            Invoke-CheckedProcess $uninstaller @("/S")
            $deadline = [DateTime]::UtcNow.AddSeconds(15)
            while ((Test-Path -LiteralPath $uninstaller) -and [DateTime]::UtcNow -lt $deadline) {
                Start-Sleep -Milliseconds 100
            }
        } catch { Write-Warning "Cleanup uninstall failed: $_" }
    }
    if ($otherHandlerCreated -and (Get-DefaultRegistryValue "Software\Classes\.sawer") -eq "SawerInstallerTest.Other") {
        [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree("Software\Classes\.sawer", $false)
    }
    $resolvedTest = [IO.Path]::GetFullPath($testRoot)
    Assert-Condition ($resolvedTest.StartsWith($temporaryRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) "Refusing cleanup outside the test temporary root"
    if (Test-Path -LiteralPath $resolvedTest) { Remove-Item -LiteralPath $resolvedTest -Recurse -Force }
    $expectedPreferences = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ApplicationData)) "Sawer\Sawer"
    Assert-Condition ([IO.Path]::GetFullPath($preferencesRoot) -eq [IO.Path]::GetFullPath($expectedPreferences)) "Refusing cleanup outside the test preferences directory"
    if (Test-Path -LiteralPath $preferencesRoot) { Remove-Item -LiteralPath $preferencesRoot -Recurse -Force }
}
