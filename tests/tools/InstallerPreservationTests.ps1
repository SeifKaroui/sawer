param([Parameter(Mandatory = $true)][string]$InstallerScript)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path -LiteralPath $InstallerScript).Path, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw ($parseErrors.Message -join [Environment]::NewLine) }
$names = @('Assert-Condition', 'Get-PreferenceHashes', 'Assert-Preserved', 'Invoke-CheckedProcess')
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
    Write-Output 'Installer preservation, exit-code, HTTPS fallback, cache and checksum regression checks passed'
} finally {
    $resolved = [IO.Path]::GetFullPath($testRoot)
    if (-not $resolved.StartsWith($temporaryRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing cleanup outside the test temporary root'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
