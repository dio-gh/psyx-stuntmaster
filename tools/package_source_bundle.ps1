[CmdletBinding()]
param(
    [string] $OutputPath = '',
    [string] $SourceCommit = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)][string] $FilePath,
        [Parameter(ValueFromRemainingArguments = $true)][string[]] $Arguments
    )
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
    }
}

function Remove-VerifiedTree {
    param(
        [Parameter(Mandatory = $true)][string] $Path,
        [Parameter(Mandatory = $true)][string] $AllowedParent
    )
    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $resolvedPath = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $resolvedParent = [IO.Path]::GetFullPath($AllowedParent).TrimEnd('\')
    if (-not $resolvedPath.StartsWith(
            "$resolvedParent\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove a path outside $resolvedParent`: $resolvedPath"
    }
    $root = Get-Item -LiteralPath $resolvedPath -Force
    $allItems = @($root) + @(Get-ChildItem -LiteralPath $resolvedPath `
        -Recurse -Force -ErrorAction Stop)
    $reparsePoints = @($allItems | Where-Object {
            ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0
        })
    if ($reparsePoints.Count -ne 0) {
        throw "Refusing to remove a tree containing a reparse point: $($reparsePoints[0].FullName)"
    }
    Remove-Item -LiteralPath $resolvedPath -Recurse -Force
}

# The source bundle's version matches the executable it accompanies: a tag
# override, or "<branch>-<shorthash>" by default (see resolve_release_version).
$ProjectVersion = & (Join-Path $PSScriptRoot 'resolve_release_version.ps1')
if (-not $SourceCommit) {
    $SourceCommit = (& git -C $RepoRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not determine the exact Stuntmaster source commit.'
    }
}
if ($SourceCommit -notmatch '^[0-9a-f]{40}$') {
    throw 'Could not determine the exact Stuntmaster source commit.'
}
$PsyCrossCommit = (& git -C $RepoRoot rev-parse "${SourceCommit}:external/PsyCross").Trim()
if ($LASTEXITCODE -ne 0 -or $PsyCrossCommit -notmatch '^[0-9a-f]{40}$') {
    throw 'Could not determine the pinned PsyCross gitlink.'
}
$WuffsCommit = (& git -C $RepoRoot rev-parse "${SourceCommit}:external/wuffs").Trim()
if ($LASTEXITCODE -ne 0 -or $WuffsCommit -notmatch '^[0-9a-f]{40}$') {
    throw 'Could not determine the pinned Wuffs gitlink.'
}
$PsyCrossRoot = Join-Path $RepoRoot 'external\PsyCross'
$checkedOutPsyCross = [string](& git -c "safe.directory=$PsyCrossRoot" `
    -C $PsyCrossRoot rev-parse HEAD)
$checkedOutPsyCross = $checkedOutPsyCross.Trim()
if ($LASTEXITCODE -ne 0 -or $checkedOutPsyCross -ne $PsyCrossCommit) {
    throw "PsyCross checkout does not match the gitlink $PsyCrossCommit."
}
$WuffsRoot = Join-Path $RepoRoot 'external\wuffs'
$checkedOutWuffs = [string](& git -c "safe.directory=$WuffsRoot" `
    -C $WuffsRoot rev-parse HEAD)
$checkedOutWuffs = $checkedOutWuffs.Trim()
if ($LASTEXITCODE -ne 0 -or $checkedOutWuffs -ne $WuffsCommit) {
    throw "Wuffs checkout does not match the gitlink $WuffsCommit."
}
$sourceEpoch = (& git -C $RepoRoot show -s --format=%ct $SourceCommit).Trim()
if ($LASTEXITCODE -ne 0 -or $sourceEpoch -notmatch '^\d+$') {
    throw 'Could not determine the source timestamp.'
}

if (-not $OutputPath) {
    $OutputPath = Join-Path $RepoRoot `
        "dist\stuntmaster-pc-$ProjectVersion-corresponding-source.zip"
} elseif (-not [IO.Path]::IsPathRooted($OutputPath)) {
    $OutputPath = Join-Path (Get-Location) $OutputPath
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$outputParent = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $outputParent | Out-Null

$workRoot = Join-Path $RepoRoot 'build\source-bundle'
$stageRoot = Join-Path $workRoot 'stage'
$mainZip = Join-Path $workRoot 'stuntmaster.zip'
$psyCrossZip = Join-Path $workRoot 'psycross.zip'
$wuffsZip = Join-Path $workRoot 'wuffs.zip'
Remove-VerifiedTree $workRoot (Join-Path $RepoRoot 'build')
New-Item -ItemType Directory -Force -Path $stageRoot | Out-Null
$bundleRootName = "stuntmaster-pc-$ProjectVersion-source"

# Stage via `git archive --format=zip` extracted with .NET rather than an
# external tar. git writes the archive itself (no colon-bearing operand ever
# reaches tar), and ZipFile extraction avoids the drive-letter remote-archive
# hang that older Windows tar implementations exhibit on hosted runners.
$git = (Get-Command git.exe -ErrorAction Stop).Source
Add-Type -AssemblyName System.IO.Compression.FileSystem
Invoke-Native $git '-C' $RepoRoot 'archive' '--format=zip' `
    "--prefix=$bundleRootName/" "--output=$mainZip" $SourceCommit
[IO.Compression.ZipFile]::ExtractToDirectory($mainZip, $stageRoot)
Invoke-Native $git '-c' "safe.directory=$PsyCrossRoot" '-C' $PsyCrossRoot `
    'archive' '--format=zip' `
    "--prefix=$bundleRootName/external/PsyCross/" `
    "--output=$psyCrossZip" $PsyCrossCommit
[IO.Compression.ZipFile]::ExtractToDirectory($psyCrossZip, $stageRoot)
Invoke-Native $git '-c' "safe.directory=$WuffsRoot" '-C' $WuffsRoot `
    'archive' '--format=zip' `
    "--prefix=$bundleRootName/external/wuffs/" `
    "--output=$wuffsZip" $WuffsCommit
[IO.Compression.ZipFile]::ExtractToDirectory($wuffsZip, $stageRoot)

$provenance = [ordered]@{
    schema_version = '1'
    project_version = $ProjectVersion
    stuntmaster_commit = $SourceCommit
    psycross_repository = 'https://github.com/neonoxd/PsyCross'
    psycross_commit = $PsyCrossCommit
    wuffs_repository = 'https://github.com/google/wuffs'
    wuffs_version = '0.3.4'
    wuffs_commit = $WuffsCommit
    source_date_epoch = $sourceEpoch
}
$provenancePath = Join-Path $stageRoot `
    "$bundleRootName\SOURCE-PROVENANCE.json"
[IO.File]::WriteAllText(
    $provenancePath,
    ($provenance | ConvertTo-Json -Depth 5),
    [Text.UTF8Encoding]::new($false))

if (Test-Path -LiteralPath $OutputPath) {
    Remove-Item -LiteralPath $OutputPath -Force
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$stream = [IO.File]::Open(
    $OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
$zip = [IO.Compression.ZipArchive]::new(
    $stream, [IO.Compression.ZipArchiveMode]::Create, $false)
$timestamp = [DateTimeOffset]::FromUnixTimeSeconds([int64]$sourceEpoch)
$minimumZipTimestamp = [DateTimeOffset]::new(
    1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
if ($timestamp -lt $minimumZipTimestamp) {
    $timestamp = $minimumZipTimestamp
}
try {
    $files = @(Get-ChildItem -LiteralPath $stageRoot -Recurse -File |
        Sort-Object FullName)
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($stageRoot.Length + 1).
            Replace('\', '/')
        $entry = $zip.CreateEntry(
            $relative, [IO.Compression.CompressionLevel]::Optimal)
        $entry.LastWriteTime = $timestamp
        $entry.ExternalAttributes = 0
        $input = [IO.File]::OpenRead($file.FullName)
        $output = $entry.Open()
        try {
            $input.CopyTo($output)
        } finally {
            $output.Dispose()
            $input.Dispose()
        }
    }
} finally {
    $zip.Dispose()
    $stream.Dispose()
}

$hash = (Get-FileHash -LiteralPath $OutputPath -Algorithm SHA256).Hash
Write-Host "Corresponding-source bundle: $OutputPath"
Write-Host "SHA-256: $hash"
Write-Output $OutputPath
