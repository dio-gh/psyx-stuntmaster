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
            'avcodec-62.dll')) {
        if (-not ($names | Where-Object { $_ -like "*/$required" })) {
            throw "Release artifact is missing $required."
        }
    }
} finally {
    $zip.Dispose()
}

$hash = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash
Write-Host "Release artifact: $artifact"
Write-Host "SHA-256: $hash"
