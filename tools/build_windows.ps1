[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string] $Configuration = 'RelWithDebInfo',
    [switch] $CoreOnly,
    [switch] $SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Some shells expose both `Path` and `PATH` in the Windows process block.
# MSBuild treats those as duplicate keys and refuses to launch cl.exe, so fold
# them into the canonical spelling before invoking any Visual Studio tool.
$processEnvironment = [System.Environment]::GetEnvironmentVariables()
$pathKeys = @($processEnvironment.Keys | Where-Object { $_ -ieq 'Path' })
if ($pathKeys.Count -gt 1) {
    $normalizedPath = [string]$processEnvironment['PATH']
    [System.Environment]::SetEnvironmentVariable(
        'PATH', $null, [System.EnvironmentVariableTarget]::Process)
    [System.Environment]::SetEnvironmentVariable(
        'Path', $normalizedPath, [System.EnvironmentVariableTarget]::Process)
}

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$git = (Get-Command git.exe -ErrorAction SilentlyContinue).Source
if (-not $git) {
    throw 'Git was not found; it is required to derive the reproducible source timestamp.'
}
$sourceDateResult = & $git -C $RepoRoot show -s --format=%ct HEAD
if ($LASTEXITCODE -ne 0) {
    throw "Could not determine SOURCE_DATE_EPOCH from the source commit."
}
$sourceDateEpoch = ([string]($sourceDateResult | Select-Object -Last 1)).Trim()
if ($sourceDateEpoch -notmatch '^\d+$') {
    throw "Could not determine SOURCE_DATE_EPOCH from the source commit."
}
$env:SOURCE_DATE_EPOCH = $sourceDateEpoch
Write-Host "Reproducible source timestamp: $sourceDateEpoch"

$Configuration = switch ($Configuration.ToLowerInvariant()) {
    'debug' { 'Debug' }
    'release' { 'Release' }
    default { 'RelWithDebInfo' }
}
$BuildRoot = Join-Path $RepoRoot $(if ($CoreOnly) {
    'build\windows-core'
} else {
    'build\windows'
})

function Invoke-Native {
    param(
        [Parameter(Mandatory = $true)]
        [string] $FilePath,
        [Parameter(ValueFromRemainingArguments = $true)]
        [string[]] $Arguments
    )
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
    }
}

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Visual Studio Installer (vswhere.exe) was not found. Install Visual Studio with the Desktop development with C++ workload.'
    }
    $result = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $result) {
        throw 'Visual Studio with the Desktop development with C++ workload was not found.'
    }
    return ([string]($result | Select-Object -Last 1)).Trim()
}

$vsRoot = Find-VisualStudio
$vswhere = Join-Path ${env:ProgramFiles(x86)} `
    'Microsoft Visual Studio\Installer\vswhere.exe'
function Get-VisualStudioProperty {
    param([Parameter(Mandatory = $true)][string] $Name)

    $result = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property $Name
    if ($LASTEXITCODE -ne 0) {
        throw "Could not query Visual Studio property $Name."
    }
    $value = $result | Select-Object -Last 1
    if ($null -eq $value) {
        return
    }
    return ([string]$value).Trim()
}

$vsInstallationVersion = Get-VisualStudioProperty 'installationVersion'
if ($vsInstallationVersion -notmatch '^(\d+)\.') {
    throw 'Could not determine the installed Visual Studio CMake generator.'
}
$vsMajorVersion = $Matches[1]
$vsReleaseYear = switch ($vsMajorVersion) {
    '16' { '2019' }
    '17' { '2022' }
    '18' { '2026' }
    default { $null }
}
if ([string]::IsNullOrWhiteSpace([string]$vsReleaseYear)) {
    # Let a future Visual Studio release describe its CMake generator year,
    # while keeping current generators independent of optional catalog fields.
    $vsReleaseYear = Get-VisualStudioProperty 'catalog_productLineVersion'
}
if ([string]::IsNullOrWhiteSpace([string]$vsReleaseYear)) {
    $vsReleaseYear = Get-VisualStudioProperty 'catalog_featureReleaseYear'
}
if ([string]::IsNullOrWhiteSpace([string]$vsReleaseYear) -or
        $vsReleaseYear -notmatch '^\d{4}$') {
    throw 'Could not determine the installed Visual Studio CMake generator.'
}
$cmakeGenerator = "Visual Studio $vsMajorVersion $vsReleaseYear"
$cmake = Join-Path $vsRoot `
    'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path -LiteralPath $cmake -PathType Leaf)) {
    $cmakeCommand = Get-Command cmake.exe -ErrorAction SilentlyContinue
    if (-not $cmakeCommand) {
        throw 'CMake was not found. Add the C++ CMake tools for Windows component to Visual Studio.'
    }
    $cmake = $cmakeCommand.Source
}

$configure = @(
    '--fresh',
    '-S', $RepoRoot,
    '-B', $BuildRoot,
    '-G', $cmakeGenerator,
    '-A', 'x64',
    '-DSTUNTMASTER_BUILD_TESTS=ON'
)

$wuffsMarker = Join-Path $RepoRoot 'external\wuffs\wuffs-root-directory.txt'
if (-not (Test-Path -LiteralPath $wuffsMarker -PathType Leaf)) {
    Write-Host 'Initializing the Wuffs submodule...'
    Invoke-Native $git '-C' $RepoRoot 'submodule' 'update' '--init' `
        'external/wuffs'
}

if ($CoreOnly) {
    $configure += '-DSTUNTMASTER_ENABLE_PSYCROSS=OFF'
} else {
    $submoduleCmake = Join-Path $RepoRoot 'external\PsyCross\CMakeLists.txt'
    if (-not (Test-Path -LiteralPath $submoduleCmake -PathType Leaf)) {
        Write-Host 'Initializing the PsyCross submodule...'
        Invoke-Native $git '-C' $RepoRoot 'submodule' 'update' '--init' `
            '--recursive'
    }
    $toolchain = Join-Path $vsRoot `
        'VC\vcpkg\scripts\buildsystems\vcpkg.cmake'
    if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
        throw 'Visual Studio vcpkg was not found. Add the vcpkg package manager component to Visual Studio.'
    }
    $configure += @(
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        "-DVCPKG_OVERLAY_TRIPLETS=$RepoRoot\triplets",
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static-release',
        '-DVCPKG_MANIFEST_INSTALL=ON',
        '-DSTUNTMASTER_ENABLE_PSYCROSS=ON'
    )
}

Write-Host "Configuring $Configuration in $BuildRoot..."
Invoke-Native $cmake @configure
Invoke-Native $cmake '--build' $BuildRoot '--config' $Configuration `
    '--target' 'ALL_BUILD'

if (-not $SkipTests) {
    $ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
    Invoke-Native $ctest '--test-dir' $BuildRoot '-C' $Configuration `
        '--output-on-failure'
}

$executable = Join-Path $BuildRoot "$Configuration\stuntmaster.exe"
Write-Host "Build complete: $executable"
