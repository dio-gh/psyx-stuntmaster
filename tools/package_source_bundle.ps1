[CmdletBinding()]
param(
    [string] $OutputPath = '',
    [string] $SourceCommit = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$FfmpegVersion = '8.1.2'
$FfmpegSha256 = '464BEB5E7BF0C311E68B45AE2F04E9CC2AF88851ABB4082231742A74D97B524C'
$FfmpegSignatureSha256 = '0A0963FCCD70597838073F3E31B20F4A4D8CC2B5E577472C9A5A1F22624246F8'
$FfmpegKeySha256 = '397B3BECEDCD5A98769967FF1FF8501DDC89F8368B8F766E4701377D7DBAABE5'
$FfmpegFingerprint = 'FCF986EA15E6E293A5644F10B4322F04D67658D8'

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

function Get-Sha256 {
    param([Parameter(Mandatory = $true)][string] $Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
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
}
if ($LASTEXITCODE -ne 0 -or $SourceCommit -notmatch '^[0-9a-f]{40}$') {
    throw 'Could not determine the exact Stuntmaster source commit.'
}
$PsyCrossCommit = (& git -C $RepoRoot rev-parse "${SourceCommit}:external/PsyCross").Trim()
if ($LASTEXITCODE -ne 0 -or $PsyCrossCommit -notmatch '^[0-9a-f]{40}$') {
    throw 'Could not determine the pinned PsyCross gitlink.'
}
$PsyCrossRoot = Join-Path $RepoRoot 'external\PsyCross'
$checkedOutPsyCross = [string](& git -c "safe.directory=$PsyCrossRoot" `
    -C $PsyCrossRoot rev-parse HEAD)
$checkedOutPsyCross = $checkedOutPsyCross.Trim()
if ($LASTEXITCODE -ne 0 -or $checkedOutPsyCross -ne $PsyCrossCommit) {
    throw "PsyCross checkout does not match the gitlink $PsyCrossCommit."
}
$sourceEpoch = (& git -C $RepoRoot show -s --format=%ct $SourceCommit).Trim()
if ($LASTEXITCODE -ne 0 -or $sourceEpoch -notmatch '^\d+$') {
    throw 'Could not determine the source timestamp.'
}

$downloadRoot = Join-Path $RepoRoot 'build\downloads'
$ffmpegFiles = [ordered]@{
    "ffmpeg-$FfmpegVersion.tar.xz" = $FfmpegSha256
    "ffmpeg-$FfmpegVersion.tar.xz.asc" = $FfmpegSignatureSha256
    'ffmpeg-devel.asc' = $FfmpegKeySha256
}
foreach ($item in $ffmpegFiles.GetEnumerator()) {
    $path = Join-Path $downloadRoot $item.Key
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Verified FFmpeg source input is missing: $path"
    }
    $actual = Get-Sha256 $path
    if ($actual -ne $item.Value) {
        throw "FFmpeg source input checksum mismatch for $($item.Key): $actual"
    }
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

$thirdPartyRoot = Join-Path $stageRoot `
    "$bundleRootName\third_party\ffmpeg"
New-Item -ItemType Directory -Force -Path $thirdPartyRoot | Out-Null
foreach ($name in $ffmpegFiles.Keys) {
    Copy-Item -LiteralPath (Join-Path $downloadRoot $name) `
        -Destination (Join-Path $thirdPartyRoot $name)
}

$provenance = [ordered]@{
    schema_version = '1'
    project_version = $ProjectVersion
    stuntmaster_commit = $SourceCommit
    psycross_repository = 'https://github.com/neonoxd/PsyCross'
    psycross_commit = $PsyCrossCommit
    ffmpeg_version = $FfmpegVersion
    ffmpeg_source_url = "https://ffmpeg.org/releases/ffmpeg-$FfmpegVersion.tar.xz"
    ffmpeg_source_sha256 = $FfmpegSha256.ToLowerInvariant()
    ffmpeg_signature_sha256 = $FfmpegSignatureSha256.ToLowerInvariant()
    ffmpeg_release_key_url = 'https://ffmpeg.org/ffmpeg-devel.asc'
    ffmpeg_release_key_sha256 = $FfmpegKeySha256.ToLowerInvariant()
    ffmpeg_release_key_fingerprint = $FfmpegFingerprint
    source_date_epoch = $sourceEpoch
    relinking_instructions = 'docs/FFMPEG_RELINKING.md'
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
