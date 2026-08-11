[CmdletBinding()]
param(
    # Explicit override; defaults to the STUNTMASTER_RELEASE_VERSION environment
    # variable. A leading "v" is stripped so a tag like "v1.2.3" yields "1.2.3".
    [string] $Override = $env:STUNTMASTER_RELEASE_VERSION
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Resolves the release version label used to name the published executable, the
# corresponding-source bundle, and the SBOM. Emits exactly one string (no other
# output), so callers can capture it with `$v = & resolve_release_version.ps1`.
#
#   - An explicit override wins (a release tag): the environment variable above
#     or -Override, with a leading "v" stripped.
#   - Otherwise the default identifies the working tree as "<branch>-<shorthash>"
#     so untagged local and CI builds are self-describing rather than a static
#     "0.0.1". The branch name comes from GITHUB_REF_NAME when set (CI checks out
#     a detached HEAD, where git cannot report it), otherwise from git.

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

function Get-SafeLabel {
    param([Parameter(Mandatory = $true)][string] $Value)
    return (($Value -replace '[^A-Za-z0-9.+~_-]+', '-').Trim('-'))
}

if ($Override) {
    $version = ($Override.Trim() -replace '^[vV]', '')
} else {
    $branch = $env:GITHUB_REF_NAME
    if (-not $branch) {
        $branch = "$(& git -C $RepoRoot rev-parse --abbrev-ref HEAD)".Trim()
        if ($LASTEXITCODE -ne 0) {
            throw 'Could not determine the current branch for the release version.'
        }
    }
    if (-not $branch -or $branch -eq 'HEAD') {
        $branch = 'detached'
    }
    $short = "$(& git -C $RepoRoot rev-parse --short HEAD)".Trim()
    if ($LASTEXITCODE -ne 0 -or -not $short) {
        throw 'Could not determine the commit short hash for the release version.'
    }
    $safeBranch = Get-SafeLabel $branch
    if (-not $safeBranch) {
        $safeBranch = 'build'
    }
    $version = "$safeBranch-$short"
}

if ($version -notmatch '^[A-Za-z0-9][A-Za-z0-9.+~_-]*$') {
    throw "Refusing an unsafe release version label: '$version'."
}

return $version
