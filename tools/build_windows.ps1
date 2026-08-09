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

function Get-PrebuiltFfmpeg {
    $version = '20260520.1.0'
    $expectedSha256 = `
        '1BBEB9FE962B3CC3782541C3F02BBB491BB03B95D6C124BFAA859CE39FAC83CF'
    $dependencyRoot = Join-Path $RepoRoot `
        "build\dependencies\ffmpeg-lgpl-$version"
    $nativeRoot = Join-Path $dependencyRoot 'build\native'
    $required = @(
        'include\libavcodec\avcodec.h',
        'lib\avcodec.lib',
        'bin\avcodec-62.dll',
        'LICENSE.txt'
    )
    $complete = $true
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $nativeRoot $relative) `
                -PathType Leaf)) {
            $complete = $false
            break
        }
    }
    if ($complete) {
        return $nativeRoot
    }

    $downloadRoot = Join-Path $RepoRoot 'build\downloads'
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
    $archive = Join-Path $downloadRoot "ffmpeg.lgpl.$version.nupkg"
    $validArchive = $false
    if (Test-Path -LiteralPath $archive -PathType Leaf) {
        $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        $validArchive = $actual -eq $expectedSha256
    }
    if (-not $validArchive) {
        if (Test-Path -LiteralPath $archive) {
            Remove-Item -LiteralPath $archive -Force
        }
        $curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
        if (-not $curl) {
            throw 'curl.exe was not found; it is included with supported Windows versions.'
        }
        $url = "https://www.nuget.org/api/v2/package/FFmpeg.LGPL/$version"
        Write-Host "Downloading prebuilt FFmpeg $version (one-time, about 150 MB)..."
        Invoke-Native $curl '--fail' '--location' '--retry' '3' `
            '--retry-all-errors' '--output' $archive $url
        $actual = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash
        if ($actual -ne $expectedSha256) {
            Remove-Item -LiteralPath $archive -Force
            throw "FFmpeg package checksum mismatch (received $actual)."
        }
    }

    if (Test-Path -LiteralPath $dependencyRoot) {
        Remove-Item -LiteralPath $dependencyRoot -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $dependencyRoot | Out-Null
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::ExtractToDirectory(
        $archive, $dependencyRoot)
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $nativeRoot $relative) `
                -PathType Leaf)) {
            throw "The FFmpeg package is missing $relative."
        }
    }
    return $nativeRoot
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
        $git = (Get-Command git.exe -ErrorAction SilentlyContinue).Source
        if (-not $git) {
            throw 'Git was not found and the PsyCross submodule is not initialized.'
        }
        Write-Host 'Initializing the PsyCross submodule...'
        Invoke-Native $git '-C' $RepoRoot 'submodule' 'update' '--init' `
            '--recursive'
    }
    $toolchain = Join-Path $vsRoot `
        'VC\vcpkg\scripts\buildsystems\vcpkg.cmake'
    if (-not (Test-Path -LiteralPath $toolchain -PathType Leaf)) {
        throw 'Visual Studio vcpkg was not found. Add the vcpkg package manager component to Visual Studio.'
    }
    $ffmpegRoot = Get-PrebuiltFfmpeg
    $configure += @(
        "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
        '-DVCPKG_TARGET_TRIPLET=x64-windows-static',
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
