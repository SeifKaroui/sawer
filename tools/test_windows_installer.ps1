[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Installer,

    [Parameter(Mandatory = $true)]
    [string]$PortableExecutable,

    [Parameter(Mandatory = $true)]
    [string]$Version,

    [Parameter(Mandatory = $true)]
    [string]$SourceRoot,

    [string]$PythonExecutable = "python"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
Add-Type -AssemblyName System.Drawing

function Assert-Condition {
    param(
        [bool]$Condition,
        [string]$Message
    )

    if (-not $Condition) {
        throw $Message
    }
}

function Invoke-CheckedProcess {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList
    )

    $process = Start-Process `
        -FilePath $FilePath `
        -ArgumentList $ArgumentList `
        -Wait `
        -PassThru
    if ($process.ExitCode -ne 0) {
        throw "$FilePath exited with code $($process.ExitCode)"
    }
}

function Invoke-CheckedCommand {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath exited with code $LASTEXITCODE"
    }
}

function Get-DefaultRegistryValue {
    param([string]$Subkey)

    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($Subkey)
    if ($null -eq $key) {
        return $null
    }
    try {
        return $key.GetValue("")
    }
    finally {
        $key.Dispose()
    }
}

function Get-DesktopDirectory {
    $desktop = [Environment]::GetFolderPath(
        [Environment+SpecialFolder]::DesktopDirectory
    )
    if (-not [string]::IsNullOrWhiteSpace($desktop)) {
        return $desktop
    }

    $desktop = Get-ItemPropertyValue `
        -LiteralPath "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\User Shell Folders" `
        -Name "Desktop"
    return [Environment]::ExpandEnvironmentVariables($desktop)
}

$installerPath = (Resolve-Path -LiteralPath $Installer).Path
$portablePath = (Resolve-Path -LiteralPath $PortableExecutable).Path
$sourcePath = (Resolve-Path -LiteralPath $SourceRoot).Path
$testName = "Sawer installer test $([char]0x03A9) $([Guid]::NewGuid())"
$temporaryRoot = if ([string]::IsNullOrWhiteSpace($env:RUNNER_TEMP)) {
    [IO.Path]::GetTempPath()
}
else {
    $env:RUNNER_TEMP
}
$testRoot = Join-Path $temporaryRoot $testName
$installRoot = Join-Path $testRoot "Installed Sawer"
$boardPath = Join-Path $testRoot "Preserve me.sawer"
$installedExecutable = Join-Path $installRoot "Sawer.exe"
$uninstaller = Join-Path $installRoot "Uninstall.exe"
$desktopShortcut = Join-Path `
    (Get-DesktopDirectory) `
    "Sawer.lnk"
$startMenuShortcut = Join-Path `
    ([Environment]::GetFolderPath([Environment+SpecialFolder]::Programs)) `
    "Sawer.lnk"

Assert-Condition `
    (-not (Test-Path -LiteralPath $desktopShortcut)) `
    "Refusing to overwrite an existing Sawer desktop shortcut"
Assert-Condition `
    (-not (Test-Path -LiteralPath $startMenuShortcut)) `
    "Refusing to overwrite an existing Sawer Start Menu shortcut"
Assert-Condition `
    (-not (Test-Path "HKCU:\Software\Classes\Sawer.Board")) `
    "Installer test requires an account without an existing Sawer installation"
Assert-Condition `
    (-not (Test-Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{49266FD7-A24C-40B7-9364-AB6615BF4510}")) `
    "Installer test requires an account without an existing Sawer uninstall entry"

New-Item -ItemType Directory -Path $testRoot | Out-Null
Set-Content -LiteralPath $boardPath -Value "installer preservation check"

try {
    $metadata = (Get-Item -LiteralPath $installerPath).VersionInfo
    Assert-Condition `
        ($metadata.ProductVersion -eq $Version) `
        "Installer ProductVersion '$($metadata.ProductVersion)' does not match '$Version'"
    Assert-Condition `
        ($metadata.FileDescription -eq "Sawer Installer") `
        "Installer FileDescription is missing"
    $icon = [System.Drawing.Icon]::ExtractAssociatedIcon($installerPath)
    Assert-Condition ($null -ne $icon) "Installer icon is missing"
    $icon.Dispose()

    Invoke-CheckedCommand $PythonExecutable @(
        (Join-Path $sourcePath "tools\check_release.py"),
        "--source-root", $sourcePath,
        "--executable", $portablePath
    )

    $installArguments = @(
        "/S",
        "/D=$installRoot"
    )
    Invoke-CheckedProcess $installerPath $installArguments

    Assert-Condition `
        (Test-Path -LiteralPath $installedExecutable -PathType Leaf) `
        "Installed Sawer executable is missing"
    Invoke-CheckedCommand $PythonExecutable @(
        (Join-Path $sourcePath "tools\check_release.py"),
        "--source-root", $sourcePath,
        "--executable", $installedExecutable
    )
    Assert-Condition `
        (Test-Path -LiteralPath $desktopShortcut -PathType Leaf) `
        "Desktop shortcut is missing"
    Assert-Condition `
        (Test-Path -LiteralPath $startMenuShortcut -PathType Leaf) `
        "Start Menu shortcut is missing"

    $openCommand = Get-DefaultRegistryValue `
        "Software\Classes\Sawer.Board\shell\open\command"
    $expectedCommand = "`"$installedExecutable`" `"%1`""
    Assert-Condition `
        ($openCommand -eq $expectedCommand) `
        "Sawer board open command is not safely quoted"
    Assert-Condition `
        ((Get-DefaultRegistryValue "Software\Classes\.sawer") -eq "Sawer.Board") `
        "The clean test account did not receive the Sawer board association"
    $registeredApplications = Get-ItemPropertyValue `
        -LiteralPath "HKCU:\Software\RegisteredApplications" `
        -Name "Sawer"
    Assert-Condition `
        ($registeredApplications -eq "Software\Sawer\Capabilities") `
        "Sawer is missing from Windows Default Apps registration"

    $upgradeArguments = @(
        "/S",
        "/D=$installRoot"
    )
    Invoke-CheckedProcess $installerPath $upgradeArguments
    Assert-Condition `
        (Test-Path -LiteralPath $boardPath -PathType Leaf) `
        "Reinstall removed a board outside the installation directory"

    Assert-Condition `
        (Test-Path -LiteralPath $uninstaller -PathType Leaf) `
        "Uninstaller is missing"
    Invoke-CheckedProcess $uninstaller @(
        "/S"
    )

    Assert-Condition `
        (-not (Test-Path -LiteralPath $installedExecutable)) `
        "Uninstall left the application executable behind"
    Assert-Condition `
        (-not (Test-Path -LiteralPath $desktopShortcut)) `
        "Uninstall left the desktop shortcut behind"
    Assert-Condition `
        (-not (Test-Path -LiteralPath $startMenuShortcut)) `
        "Uninstall left the Start Menu shortcut behind"
    Assert-Condition `
        (-not (Test-Path -LiteralPath $installRoot)) `
        "Uninstall left the application directory behind"
    Assert-Condition `
        ($null -eq (Get-DefaultRegistryValue "Software\Classes\Sawer.Board")) `
        "Uninstall left the Sawer ProgID behind"
    Assert-Condition `
        ($null -eq (Get-DefaultRegistryValue "Software\Classes\.sawer")) `
        "Uninstall left Sawer as the default board handler"
    Assert-Condition `
        (-not (Test-Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\{49266FD7-A24C-40B7-9364-AB6615BF4510}")) `
        "Uninstall left its Apps entry behind"
    $registeredAfterUninstall = Get-ItemProperty `
        -LiteralPath "HKCU:\Software\RegisteredApplications" `
        -Name "Sawer" `
        -ErrorAction SilentlyContinue
    Assert-Condition `
        ($null -eq $registeredAfterUninstall) `
        "Uninstall left Sawer registered with Default Apps"
    Assert-Condition `
        (Test-Path -LiteralPath $boardPath -PathType Leaf) `
        "Uninstall removed a user board"
}
finally {
    if (Test-Path -LiteralPath $uninstaller -PathType Leaf) {
        try {
            Invoke-CheckedProcess $uninstaller @(
                "/S"
            )
        }
        catch {
            Write-Warning "Cleanup uninstall failed: $_"
        }
    }
    if (Test-Path -LiteralPath $testRoot) {
        Remove-Item -LiteralPath $testRoot -Recurse -Force
    }
}
