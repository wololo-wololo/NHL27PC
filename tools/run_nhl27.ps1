[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $GameDirectory,

    [Parameter(Mandatory = $true)]
    [ValidateNotNullOrEmpty()]
    [string] $KytyDirectory,

    [switch] $ShaderCache,

    [switch] $Optimized,

    # Experimental: leave dispatch-to-dispatch ordering to the game's own GPU synchronization.
    [switch] $RelaxedComputeBarriers,

    # Appends per-second performance counters to this file (no overlay needed).
    [string] $PerfLog,

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
    '--no-splash'
)

if ($Optimized) {
    # Guest printf is extremely chatty in NHL 27 and can write tens of MB/s during a match.
    # Keep the performance preset free of synchronous formatting and file-I/O overhead.
    $arguments += @('--printf-direction', 'Silent')
} else {
    $arguments += @(
        '--printf-direction', 'File',
        '--printf-output-file', 'nhl27-guest.log'
    )
}

if ($ShaderCache) {
    $arguments += '--shader-cache'
}

if ($Optimized) {
    if (-not $ShaderCache) {
        $arguments += '--shader-cache'
    }

    $arguments += @(
        '--shader-optimization-type', 'None',
        '--screen-width', '1280',
        '--screen-height', '720',
        '--present-mode', 'Mailbox'
    )
}

$arguments += $EmulatorArguments

Write-Host "Launching NHL 27 $($metadata.contentVersion) with $emulatorPath"
Write-Host 'Game data remains outside the repository.'
if ($Optimized) {
    Write-Host 'Optimized mode: silent guest printf, warm compatible shader cache, 720p Mailbox, no implicit Vulkan overlays.'
}

$exitCode = 0
$previousLayerFilter = [Environment]::GetEnvironmentVariable('VK_LOADER_LAYERS_DISABLE', 'Process')
Push-Location -LiteralPath $kytyPath
try {
    if ($Optimized) {
        $filters = @($previousLayerFilter -split ',' | Where-Object { $_ })
        if ($filters -notcontains '~implicit~') {
            $filters += '~implicit~'
        }
        [Environment]::SetEnvironmentVariable(
            'VK_LOADER_LAYERS_DISABLE',
            ($filters -join ','),
            'Process'
        )
    }

    if ($RelaxedComputeBarriers) {
        $env:KYTY_RELAXED_CS_BARRIERS = '1'
    }
    if ($PerfLog) {
        $env:KYTY_PERF_CSV = [IO.Path]::GetFullPath((Join-Path $PWD $PerfLog))
    }

    & $emulatorPath @arguments
    $exitCode = $LASTEXITCODE
} finally {
    if ($RelaxedComputeBarriers) {
        Remove-Item Env:KYTY_RELAXED_CS_BARRIERS -ErrorAction SilentlyContinue
    }
    if ($PerfLog) {
        Remove-Item Env:KYTY_PERF_CSV -ErrorAction SilentlyContinue
    }
    [Environment]::SetEnvironmentVariable(
        'VK_LOADER_LAYERS_DISABLE',
        $previousLayerFilter,
        'Process'
    )
    Pop-Location
}

exit $exitCode
