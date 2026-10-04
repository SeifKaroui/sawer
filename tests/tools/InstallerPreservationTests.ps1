param([Parameter(Mandatory = $true)][string]$InstallerScript)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path -LiteralPath $InstallerScript).Path, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors.Message -join [Environment]::NewLine) }
$names = @('Assert-Condition', 'Get-PreferenceHashes', 'Assert-Preserved', 'Invoke-CheckedProcess', 'Assert-Shortcut')
foreach ($definition in $ast.FindAll({param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $false)) {
    if ($definition.Name -in $names) { Invoke-Expression $definition.Extent.Text }
}
function Assert-Rejected([scriptblock]$Action, [string]$ExpectedMessage) {
    $rejected = $false
    try { & $Action } catch {
        if ($_.Exception.Message -notlike $ExpectedMessage) { throw }
        $rejected = $true
    }
    if (-not $rejected) { throw 'Corrupted data or an unexpected exit was accepted' }
}
$temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$testRoot = Join-Path $temporaryRoot ('sawer-installer-helpers-' + [Guid]::NewGuid())
$preferencesRoot = Join-Path $testRoot 'preferences'
$boardPath = Join-Path $testRoot 'board.sawer'
try {
    New-Item -ItemType Directory -Path $preferencesRoot -Force | Out-Null
    [IO.File]::WriteAllBytes($boardPath, [byte[]]@(1, 2, 3, 4))
    $preference = Join-Path $preferencesRoot "$([char]0x753b)$([char]0x677f).json"
    [IO.File]::WriteAllBytes($preference, [byte[]]@(5, 6, 7, 8))
    $boardHash = (Get-FileHash -LiteralPath $boardPath -Algorithm SHA256).Hash
    $before = Get-PreferenceHashes
    Assert-Condition ($before.Count -eq 1) 'Unicode preference file was omitted'
    Assert-Preserved $before
    $previewDirectory = Join-Path $preferencesRoot 'previews'
    New-Item -ItemType Directory -Path $previewDirectory | Out-Null
    $preview = Join-Path $previewDirectory 'board-preview.cache'
    [IO.File]::WriteAllBytes($preview, [byte[]]@(1, 2))
    $logLock = [IO.File]::Open((Join-Path $preferencesRoot 'Sawer.log'),
        [IO.FileMode]::Create, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    try {
        $snapshot = Get-PreferenceHashes
        Assert-Condition ($snapshot.Count -eq 1) 'Runtime log or disposable previews were included in preference hashes'
        $logLock.WriteByte(42)
        [IO.File]::WriteAllBytes($preview, [byte[]]@(3, 4))
        Assert-Preserved $snapshot
        # Similar directory and file names must still be protected as user data.
        $nestedDirectory = Join-Path $preferencesRoot 'previews-backup'
        New-Item -ItemType Directory -Path $nestedDirectory | Out-Null
        $nestedPreference = Join-Path $nestedDirectory 'Sawer.log'
        [IO.File]::WriteAllBytes($nestedPreference, [byte[]]@(5, 6))
        $nestedSnapshot = Get-PreferenceHashes
        Assert-Condition ($nestedSnapshot.Count -eq 2 -and $nestedSnapshot.ContainsKey($nestedPreference)) 'Exclusions hid unrelated nested user data'
        [IO.File]::WriteAllBytes($nestedPreference, [byte[]]@(7, 8))
        Assert-Rejected { Assert-Preserved $nestedSnapshot } 'Preferences changed:*'
    } finally { $logLock.Dispose() }
    $preferenceLock = [IO.File]::Open($preference, [IO.FileMode]::Open,
        [IO.FileAccess]::Read, [IO.FileShare]::None)
    try {
        $lockedPreferenceRejected = $false
        try { Get-PreferenceHashes | Out-Null } catch { $lockedPreferenceRejected = $true }
        Assert-Condition $lockedPreferenceRejected 'An unreadable user preference was silently skipped'
    } finally { $preferenceLock.Dispose() }
    [IO.File]::WriteAllBytes($preference, [byte[]]@(5, 6, 7, 9))
    Assert-Rejected { Assert-Preserved $before } 'Preferences changed:*'
    [IO.File]::WriteAllBytes($preference, [byte[]]@(5, 6, 7, 8))
    [IO.File]::WriteAllBytes($boardPath, [byte[]]@(1, 2, 3, 5))
    Assert-Rejected { Assert-Preserved $before } 'Installer operation changed or removed the board'
    [IO.File]::WriteAllBytes($boardPath, [byte[]]@(1, 2, 3, 4))
    Remove-Item -LiteralPath $preference
    Assert-Rejected { Assert-Preserved $before } 'Preferences were removed:*'
    $helper = (Get-Process -Id $PID).Path
    Invoke-CheckedProcess $helper @('-NoProfile', '-Command', '"exit 2"') 2
    Assert-Rejected {
        Invoke-CheckedProcess $helper @('-NoProfile', '-Command', '"exit 2"')
    } '*exited with 2, expected 0'
    # Build a real Shell link with a Unicode target and filename, without installing Sawer.
    $unicodeDirectory = Join-Path $testRoot "$([char]0x753b)$([char]0x677f) shortcuts"
    New-Item -ItemType Directory -Path $unicodeDirectory | Out-Null
    $targetExecutable = Join-Path $unicodeDirectory 'Sawer.exe'
    Copy-Item -LiteralPath $helper -Destination $targetExecutable
    $seedPath = Join-Path $testRoot 'seed.lnk'
    $wscript = New-Object -ComObject WScript.Shell
    $seed = $wscript.CreateShortcut($seedPath)
    $seed.TargetPath = $helper
    $seed.Save()
    $shell = New-Object -ComObject Shell.Application
    $link = $shell.Namespace($testRoot).ParseName('seed.lnk').GetLink
    $link.Path = $targetExecutable
    $link.Arguments = ''
    $shortcutPath = Join-Path $unicodeDirectory "$([char]0x753b)$([char]0x677f).lnk"
    $link.Save($shortcutPath)
    Assert-Shortcut $shortcutPath $targetExecutable
    Assert-Shortcut $shortcutPath ($targetExecutable.Replace('\', '/').ToUpperInvariant())
    Assert-Rejected { Assert-Shortcut $shortcutPath $helper } 'Shortcut targets a different application:*'
    $link.Arguments = '--unexpected'
    $link.Save($shortcutPath)
    Assert-Rejected { Assert-Shortcut $shortcutPath $targetExecutable } 'Shortcut contains unexpected arguments:*'
    Assert-Rejected { Assert-Shortcut (Join-Path $unicodeDirectory 'missing.lnk') $targetExecutable } 'Shortcut is missing:*'
    # Execute the workflow's actual download block with simulated transfers.
    $sourceRoot = Split-Path (Split-Path (Resolve-Path -LiteralPath $InstallerScript).Path -Parent) -Parent
    $workflow = Get-Content (Join-Path $sourceRoot '.github/workflows/windows-release.yml') -Raw
    $step = [regex]::Match($workflow, '(?ms)^      - name: Set up pinned NSIS compiler\r?\n.*?        run: \|\r?\n(?<body>.*?)(?=^      - name:)')
    Assert-Condition $step.Success 'NSIS setup step was not found'
    $setup = [regex]::Replace($step.Groups['body'].Value, '(?m)^          ', '')
    $download = $setup.Substring(0, $setup.IndexOf('$compilerDirectory ='))
    $download = [regex]::Replace($download, "(?m)^\`$expected = '[0-9a-f]+'", ('$expected = ''' + $boardHash + ''''))
    $previousNsisCache = $env:NSIS_CACHE_DIR
    function curl.exe {
        param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
        $script:downloadCalls++
        Assert-Condition ($Arguments -contains '--proto' -and $Arguments -contains '--proto-redir') 'Download must restrict both initial and redirect protocols'
        Assert-Condition (@($Arguments | Where-Object { $_ -eq '=https' }).Count -eq 2) 'Download allowed a non-HTTPS protocol'
        if (-not $script:badDownload -and $script:downloadCalls -eq 1) {
            $global:LASTEXITCODE = 22
            return
        }
        $output = $Arguments[[Array]::IndexOf($Arguments, '--output') + 1]
        $bytes = if ($script:badDownload) { [byte[]]@(9, 9) } else { [byte[]]@(1, 2, 3, 4) }
        [IO.File]::WriteAllBytes($output, $bytes)
        $global:LASTEXITCODE = 0
    }
    try {
        $env:NSIS_CACHE_DIR = Join-Path $testRoot 'nsis-cache'
        $script:downloadCalls = 0
        $script:badDownload = $false
        & ([scriptblock]::Create($download))
        Assert-Condition ($script:downloadCalls -eq 2) 'Download did not fall back after a transport failure'
        $script:downloadCalls = 0
        & ([scriptblock]::Create($download))
        Assert-Condition ($script:downloadCalls -eq 0) 'Verified cached NSIS archive triggered a download'
        [IO.File]::WriteAllBytes((Join-Path $env:NSIS_CACHE_DIR 'nsis-3.12.zip'), [byte[]]@(0))
        $script:badDownload = $true
        Assert-Rejected { & ([scriptblock]::Create($download)) } 'Could not download the checksum-verified NSIS archive over HTTPS'
        Assert-Condition ($script:downloadCalls -eq 2) 'Invalid cached NSIS archive did not trigger mirror downloads'
    } finally {
        $env:NSIS_CACHE_DIR = $previousNsisCache
        Remove-Item Function:\curl.exe
    }
    # Run the actual upgrade-download step with legacy and versioned release fixtures.
    $upgradeStep = [regex]::Match($workflow, '(?ms)^      - name: Retrieve a previous installer for upgrade checks\r?\n.*?        run: \|\r?\n(?<body>.*?)(?=^      - name:)')
    Assert-Condition $upgradeStep.Success 'Installer upgrade step was not found'
    $upgradeBody = [regex]::Replace($upgradeStep.Groups['body'].Value, '(?m)^          ', '')
    $upgrade = [scriptblock]::Create($upgradeBody)
    $previousLocation = Get-Location
    $previousVersionEnv = $env:VERSION
    $previousRepoEnv = $env:GH_REPO
    $previousOutputEnv = $env:GITHUB_OUTPUT
    function gh {
        param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Arguments)
        $global:LASTEXITCODE = 0
        if ($Arguments[0] -eq 'api') {
            ConvertTo-Json -InputObject @($script:upgradeReleases) -Depth 5
            return
        }
        Assert-Condition ($Arguments[0] -eq 'release' -and $Arguments[1] -eq 'download') 'Unexpected GitHub command'
        Assert-Condition ($Arguments[2] -eq $script:upgradeTag) 'Upgrade did not select the latest eligible lower release'
        $patterns = @()
        for ($index = 0; $index -lt $Arguments.Count; ++$index) {
            if ($Arguments[$index] -eq '--pattern') { $patterns += $Arguments[$index + 1] }
        }
        Assert-Condition ($patterns.Count -eq 2 -and $patterns[0] -ceq $script:upgradeInstaller -and
            $patterns[1] -ceq $script:upgradeChecksums) 'Download patterns do not match the selected release'
        $destination = $Arguments[[Array]::IndexOf($Arguments, '--dir') + 1]
        $destination = Join-Path (Get-Location).Path $destination
        $bytes = if ($script:upgradeCorrupt) { [byte[]]@(9, 9) } else { [byte[]]@(1, 2, 3, 4) }
        [IO.File]::WriteAllBytes((Join-Path $destination $script:upgradeInstaller), $bytes)
        $line = "$($boardHash.ToLowerInvariant())  $script:upgradeEntryName"
        $lines = if ($script:upgradeDuplicate) { @($line, $line) } else { @($line) }
        [IO.File]::WriteAllLines((Join-Path $destination $script:upgradeChecksums), $lines)
    }
    try {
        $legacy = @{tag_name = 'v0.8.0'; draft = $false; assets = @(
            @{name = 'Sawer-Setup.exe'}, @{name = 'SHA256SUMS.txt'})}
        $versioned = @{tag_name = 'v0.9.0'; draft = $false; assets = @(
            @{name = 'sawer-v0.9.0-windows-x64-setup.exe'},
            @{name = 'sawer-v0.9.0-windows-x64-sha256sums.txt'})}
        $incomplete = @{tag_name = 'v0.9.1'; draft = $false; assets = @(
            @{name = 'sawer-v0.9.1-windows-x64-setup.exe'})}
        $current = @{tag_name = 'v0.10.0'; draft = $false; assets = $versioned.assets}
        $draft = @{tag_name = 'v0.9.2'; draft = $true; assets = $versioned.assets}
        $mixed = @{tag_name = 'v0.9.0'; draft = $false; assets = @(
            @{name = 'sawer-v0.9.0-windows-x64-setup.exe'},
            @{name = 'Sawer-Setup.exe'}, @{name = 'SHA256SUMS.txt'})}
        $env:VERSION = '0.10.0'
        $env:GH_REPO = 'fixture/sawer'
        foreach ($scenario in @('legacy', 'versioned', 'mixed', 'wrong-name', 'duplicate', 'corrupt')) {
            $scenarioRoot = Join-Path $testRoot ('upgrade-' + $scenario)
            New-Item -ItemType Directory -Path $scenarioRoot | Out-Null
            Set-Location -LiteralPath $scenarioRoot
            $env:GITHUB_OUTPUT = Join-Path $scenarioRoot 'output.txt'
            $script:upgradeTag = if ($scenario -eq 'legacy') { 'v0.8.0' } else { 'v0.9.0' }
            $script:upgradeInstaller = if ($scenario -in @('legacy', 'mixed')) {
                'Sawer-Setup.exe'
            } else { 'sawer-v0.9.0-windows-x64-setup.exe' }
            $script:upgradeChecksums = if ($scenario -in @('legacy', 'mixed')) {
                'SHA256SUMS.txt'
            } else { 'sawer-v0.9.0-windows-x64-sha256sums.txt' }
            $script:upgradeEntryName = if ($scenario -eq 'wrong-name') {
                'sawer-v0X9X0-windows-x64-setup.exe'
            } else { $script:upgradeInstaller }
            $script:upgradeDuplicate = $scenario -eq 'duplicate'
            $script:upgradeCorrupt = $scenario -eq 'corrupt'
            $script:upgradeReleases = @($legacy, $incomplete, $current, $draft)
            if ($scenario -eq 'mixed') { $script:upgradeReleases += $mixed }
            elseif ($scenario -ne 'legacy') { $script:upgradeReleases += $versioned }
            if ($scenario -in @('wrong-name', 'duplicate')) {
                Assert-Rejected { & $upgrade } 'Previous installer checksum is missing or ambiguous'
            } elseif ($scenario -eq 'corrupt') {
                Assert-Rejected { & $upgrade } 'Previous installer checksum mismatch'
            } else {
                & $upgrade
                $output = Get-Content -LiteralPath $env:GITHUB_OUTPUT
                $expectedInstaller = (Resolve-Path -LiteralPath (Join-Path $scenarioRoot ('out/installer-upgrade/' + $script:upgradeInstaller))).Path
                Assert-Condition ($output -contains "installer=$expectedInstaller") 'Upgrade output contains the wrong installer path'
                Assert-Condition ($output -contains "version=$($script:upgradeTag.Substring(1))") 'Upgrade output contains the wrong version'
            }
        }
    } finally {
        Set-Location -LiteralPath $previousLocation.Path
        $env:VERSION = $previousVersionEnv
        $env:GH_REPO = $previousRepoEnv
        $env:GITHUB_OUTPUT = $previousOutputEnv
        Remove-Item Function:\gh
    }
    # Generate the real release checksum manifest with the versioned download names.
    $checksumStep = [regex]::Match($workflow, '(?ms)^      - name: Create release checksums\r?\n.*?        run: \|\r?\n(?<body>.*?)(?=^      - name:)')
    Assert-Condition $checksumStep.Success 'Windows checksum step was not found'
    $checksumBody = [regex]::Replace($checksumStep.Groups['body'].Value, '(?m)^          ', '')
    $previousLocation = Get-Location
    $previousStem = $env:RELEASE_STEM
    try {
        $checksumRoot = Join-Path $testRoot 'release-checksums'
        $distribution = Join-Path $checksumRoot 'dist'
        New-Item -ItemType Directory -Path $distribution -Force | Out-Null
        Set-Location -LiteralPath $checksumRoot
        $env:RELEASE_STEM = 'sawer-v0.9.0-windows-x64'
        $setupName = "$($env:RELEASE_STEM)-setup.exe"
        $portableName = "$($env:RELEASE_STEM)-portable.exe"
        $noticesName = "$($env:RELEASE_STEM)-third-party-notices.md"
        [IO.File]::WriteAllBytes((Join-Path $distribution $setupName), [byte[]]@(1, 2, 3, 4))
        [IO.File]::WriteAllBytes((Join-Path $distribution $portableName), [byte[]]@(5, 6, 7, 8))
        [IO.File]::WriteAllBytes((Join-Path $distribution $noticesName), [byte[]]@(9, 10))
        & ([scriptblock]::Create($checksumBody))
        $manifest = Join-Path $distribution "$($env:RELEASE_STEM)-sha256sums.txt"
        $lines = Get-Content -LiteralPath $manifest
        Assert-Condition ($lines.Count -eq 3) 'Release checksum manifest omitted a download'
        foreach ($name in @($setupName, $portableName, $noticesName)) {
            $hash = (Get-FileHash -LiteralPath (Join-Path $distribution $name) -Algorithm SHA256).Hash.ToLowerInvariant()
            Assert-Condition ($lines -ccontains "$hash  $name") 'Checksum entry does not match the versioned filename and bytes'
        }
        $bytes = [IO.File]::ReadAllBytes($manifest)
        Assert-Condition (-not ($bytes[0] -eq 0xef -and $bytes[1] -eq 0xbb -and $bytes[2] -eq 0xbf)) 'Checksum manifest must not have a UTF-8 BOM'
    } finally {
        Set-Location -LiteralPath $previousLocation.Path
        $env:RELEASE_STEM = $previousStem
    }
    Write-Output 'Installer preservation, Unicode shortcuts, exit-code, HTTPS cache, versioned upgrade and checksum regression checks passed'
} finally {
    $resolved = [IO.Path]::GetFullPath($testRoot)
    if (-not $resolved.StartsWith($temporaryRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing cleanup outside the test temporary root'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
