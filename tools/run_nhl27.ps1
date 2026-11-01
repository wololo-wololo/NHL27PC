[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $GameDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $KytyDirectory,

    [switch] $ShaderCache,

    [string[]] $EmulatorArguments = @()
)

$ErrorActionPreference = 'Stop'
$expectedTitleId = 'PPSA34063'

$gamePath = (Resolve-Path -LiteralPath $GameDirectory).Path
$kytyPath = (Resolve-Path -LiteralPath $KytyDirectory).Path
$emulatorPath = Join-Path $kytyPath 'kyty_emulator.exe'
$metadataPath = Join-Path $gamePath 'sce_sys\param.json'
$ebootPath = Join-Path $gamePath 'eboot.bin'

if (-not (Test-Path -LiteralPath $emulatorPath -PathType Leaf)) {
    throw "Kyty emulator not found: $emulatorPath"
}

if (-not (Test-Path -LiteralPath $metadataPath -PathType Leaf)) {
    throw "PS5 metadata not found: $metadataPath"
}

if (-not (Test-Path -LiteralPath $ebootPath -PathType Leaf)) {
    throw "PS5 executable not found: $ebootPath"
}

$metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
if ($metadata.titleId -ne $expectedTitleId) {
    throw "Expected NHL 27 title ID $expectedTitleId, found '$($metadata.titleId)'."
}

$arguments = @(
    '--game', $gamePath,
    '--no-splash',
    '--printf-direction', 'File',
    '--printf-output-file', 'nhl27-guest.log'
)

if ($ShaderCache) {
    $arguments += '--shader-cache'
}

$arguments += $EmulatorArguments

Write-Host "Launching NHL 27 $($metadata.contentVersion) with $emulatorPath"
Write-Host 'Game data remains outside the repository.'

$exitCode = 0
Push-Location -LiteralPath $kytyPath
try {
    & $emulatorPath @arguments
    $exitCode = $LASTEXITCODE
} finally {
    Pop-Location
}

exit $exitCode
