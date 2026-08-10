[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildRoot = Join-Path $RepoRoot 'build\windows'

& (Join-Path $PSScriptRoot 'build_windows.ps1') -Configuration Release
if ($LASTEXITCODE -ne 0) {
    throw "Release build failed with exit code $LASTEXITCODE."
}

$cache = Join-Path $BuildRoot 'CMakeCache.txt'
$cmakeLine = Get-Content -LiteralPath $cache | Where-Object {
    $_ -like 'CMAKE_COMMAND:INTERNAL=*'
} | Select-Object -First 1
if (-not $cmakeLine) {
    throw 'Could not find CMake in the configured release build.'
}
$cmake = $cmakeLine.Substring($cmakeLine.IndexOf('=') + 1)
& $cmake --build $BuildRoot --config Release --target package
if ($LASTEXITCODE -ne 0) {
    throw "Packaging failed with exit code $LASTEXITCODE."
}

$cpackConfig = Get-Content -LiteralPath (Join-Path $BuildRoot 'CPackConfig.cmake') `
    -Raw
if ($cpackConfig -notmatch 'set\(CPACK_PACKAGE_FILE_NAME "([^"]+)"\)') {
    throw 'Could not determine the generated CPack package name.'
}
$artifact = Join-Path $RepoRoot "dist\$($Matches[1]).zip"
if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
    throw "Expected release artifact was not produced: $artifact"
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($artifact)
try {
    $names = @($zip.Entries | ForEach-Object { $_.FullName })
    foreach ($required in @(
            'stuntmaster.exe',
            'stuntmaster-launcher.exe',
            'input.ini',
            'README-FIRST.txt',
            'licenses/FFmpeg-LGPL-2.1-or-later.txt')) {
        if (-not ($names | Where-Object { $_ -like "*/$required" })) {
            throw "Release artifact is missing $required."
        }
    }
    $ffmpegDlls = @($names | Where-Object {
        $_ -match '(?i)/(avcodec|avformat|avutil|swresample|swscale)-[0-9]+\.dll$'
    })
    if ($ffmpegDlls.Count -ne 0) {
        throw "Static release unexpectedly contains FFmpeg DLLs: $($ffmpegDlls -join ', ')"
    }
} finally {
    $zip.Dispose()
}

$artifactName = [IO.Path]::GetFileName($artifact)
if ($artifactName -notmatch '^stuntmaster-pc-(.+)-windows-x64\.zip$') {
    throw "Could not derive the corresponding-source name from $artifactName."
}
$sourceBundle = Join-Path $RepoRoot `
    "dist\stuntmaster-pc-$($Matches[1])-corresponding-source.zip"
& (Join-Path $PSScriptRoot 'package_source_bundle.ps1') `
    -OutputPath $sourceBundle
if ($LASTEXITCODE -ne 0 -or
        -not (Test-Path -LiteralPath $sourceBundle -PathType Leaf)) {
    throw 'Corresponding-source packaging failed.'
}

$hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash
Write-Host "Release artifact: $artifact"
Write-Host "SHA-256: $hash"
