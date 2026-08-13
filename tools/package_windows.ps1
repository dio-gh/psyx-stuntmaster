[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildRoot = Join-Path $RepoRoot 'build\windows'
$DistRoot = Join-Path $RepoRoot 'dist'

& (Join-Path $PSScriptRoot 'build_windows.ps1') -Configuration Release
if ($LASTEXITCODE -ne 0) {
    throw "Release build failed with exit code $LASTEXITCODE."
}

# The release artifact is the single, self-configuring executable -- no CPack
# archive, no launcher, no side files. Resolve the release version label (a tag
# override, or "<branch>-<shorthash>" by default) and publish it to the child
# source-bundle script so its version matches the executable's.
$version = & (Join-Path $PSScriptRoot 'resolve_release_version.ps1')
$env:STUNTMASTER_RELEASE_VERSION = $version

$builtExe = Join-Path $BuildRoot 'Release\stuntmaster.exe'
if (-not (Test-Path -LiteralPath $builtExe -PathType Leaf)) {
    throw "Release build did not produce $builtExe."
}

New-Item -ItemType Directory -Path $DistRoot -Force | Out-Null
$artifact = Join-Path $DistRoot "stuntmaster-pc-$version-windows-x64.exe"
Copy-Item -LiteralPath $builtExe -Destination $artifact -Force
if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
    throw "Failed to stage the release executable at $artifact."
}

# The corresponding-source bundle ships alongside the executable and mirrors
# its version.
$sourceBundle = Join-Path $DistRoot `
    "stuntmaster-pc-$version-corresponding-source.zip"
& (Join-Path $PSScriptRoot 'package_source_bundle.ps1') `
    -OutputPath $sourceBundle
if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath $sourceBundle -PathType Leaf)) {
    throw 'Corresponding-source packaging failed.'
}

$hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash
Write-Host "Release artifact: $artifact"
Write-Host "SHA-256: $hash"
