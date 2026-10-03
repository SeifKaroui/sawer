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
    Write-Output 'Installer preservation and exit-code regression checks passed'
} finally {
    $resolved = [IO.Path]::GetFullPath($testRoot)
    if (-not $resolved.StartsWith($temporaryRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Refusing cleanup outside the test temporary root'
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
