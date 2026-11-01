[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$maxTrackedBytes = 50MB
$forbiddenExtensions = @(
    '.iso', '.pkg', '.prx', '.rap', '.rif', '.self', '.sprx', '.zar'
)
$forbiddenNames = @(
    'eboot.bin', 'keystone', 'license.dat', 'nptitle.dat', 'param.sfo'
)
$forbiddenPath = [regex]'(?i)(^|/)(PPSA\d+|CUSA\d+|sce_sys|sce_module|fakelib|_DownloadData|_PipelineCache|_SaveData|_TempData)(/|$)'

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$tracked = @(& git -C $repoRoot ls-files)
if ($LASTEXITCODE -ne 0) {
    throw 'Unable to enumerate tracked files.'
}

$violations = [System.Collections.Generic.List[string]]::new()
foreach ($relativePath in $tracked) {
    $gitPath = $relativePath.Replace('\', '/')
    $leaf = [IO.Path]::GetFileName($gitPath)
    $extension = [IO.Path]::GetExtension($leaf)

    if ($forbiddenPath.IsMatch($gitPath)) {
        $violations.Add("forbidden path: $gitPath")
    }

    if ($forbiddenNames -contains $leaf.ToLowerInvariant()) {
        $violations.Add("forbidden filename: $gitPath")
    }

    if ($forbiddenExtensions -contains $extension.ToLowerInvariant()) {
        $violations.Add("forbidden extension: $gitPath")
    }

    $fullPath = Join-Path $repoRoot $relativePath
    if ((Test-Path -LiteralPath $fullPath -PathType Leaf) -and
        (Get-Item -LiteralPath $fullPath).Length -gt $maxTrackedBytes) {
        $violations.Add("tracked file exceeds 50 MiB: $gitPath")
    }
}

if ($violations.Count -gt 0) {
    $violations | Sort-Object -Unique | ForEach-Object { Write-Error $_ }
    throw 'Repository safety audit failed. Remove game/runtime data from Git history before pushing.'
}

Write-Host "Repository safety audit passed for $($tracked.Count) tracked paths."
