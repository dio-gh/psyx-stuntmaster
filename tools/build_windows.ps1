[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet('Debug', 'RelWithDebInfo', 'Release')]
    [string] $Configuration = 'RelWithDebInfo',
    [switch] $CoreOnly,
    [switch] $SkipTests,
    [switch] $DisableFfmpegAssembly,
    [switch] $ForceFfmpegRebuild
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
$sourceDateEpoch = (& $git -C $RepoRoot show -s --format=%ct HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $sourceDateEpoch -notmatch '^\d+$') {
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
        throw 'Visual Studio Installer (vswhere.exe) was not found. Install Visual Studio 2022 with the Desktop development with C++ workload.'
    }
    $result = & $vswhere -latest -products '*' `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath
    if ($LASTEXITCODE -ne 0 -or -not $result) {
        throw 'Visual Studio 2022 with the Desktop development with C++ workload was not found.'
    }
    return ([string]($result | Select-Object -Last 1)).Trim()
}

$vsRoot = Find-VisualStudio
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
    '-G', 'Visual Studio 17 2022',
    '-A', 'x64',
    '-DSTUNTMASTER_BUILD_TESTS=ON'
)

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
    $ffmpegBuild = Join-Path $PSScriptRoot 'build_ffmpeg_windows.ps1'
    $ffmpegArguments = @{
        VisualStudioRoot = $vsRoot
    }
    if ($DisableFfmpegAssembly) {
        $ffmpegArguments['DisableAssembly'] = $true
    }
    if ($ForceFfmpegRebuild) {
        $ffmpegArguments['ForceRebuild'] = $true
    }
    & $ffmpegBuild @ffmpegArguments
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg build failed with exit code $LASTEXITCODE."
    }
    $ffmpegRoot = Join-Path $RepoRoot `
        'build\dependencies\ffmpeg-8.1.2-msvc-x64-static\install'
    $configure += @(
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        "-DVCPKG_OVERLAY_TRIPLETS=$RepoRoot\triplets",
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static-release',
        '-DVCPKG_MANIFEST_INSTALL=ON',
        '-DSTUNTMASTER_ENABLE_PSYCROSS=ON',
        "-DSTUNTMASTER_FFMPEG_ROOT=$ffmpegRoot"
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
