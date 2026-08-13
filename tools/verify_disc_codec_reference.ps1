[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$CuePath,

    [Parameter(Mandatory = $true)]
    [string]$ProbePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repositoryRoot = Split-Path -Parent $PSScriptRoot
$referencePath = Join-Path $repositoryRoot `
    'tests/data/ffmpeg-8.1.2-supported-disc-codec-reference.txt'

if (-not (Test-Path -LiteralPath $CuePath -PathType Leaf)) {
    throw "CUE file not found: $CuePath"
}
if (-not (Test-Path -LiteralPath $ProbePath -PathType Leaf)) {
    throw "Codec probe not found: $ProbePath"
}
if (-not (Test-Path -LiteralPath $referencePath -PathType Leaf)) {
    throw "Codec reference not found: $referencePath"
}

$resolvedCue = (Resolve-Path -LiteralPath $CuePath).Path
$resolvedProbe = (Resolve-Path -LiteralPath $ProbePath).Path
$expected = [string[]]@(
    Get-Content -LiteralPath $referencePath |
        Where-Object {
            -not [string]::IsNullOrWhiteSpace($_) -and
            -not $_.TrimStart().StartsWith('#')
        }
)

$stderrPath = [System.IO.Path]::GetTempFileName()
try {
    $actual = [string[]]@(& $resolvedProbe $resolvedCue 2> $stderrPath)
    $probeExitCode = $LASTEXITCODE
    if ($probeExitCode -ne 0) {
        $stderr = (Get-Content -LiteralPath $stderrPath) -join [Environment]::NewLine
        throw "Codec probe failed with exit code $probeExitCode.$([Environment]::NewLine)$stderr"
    }
} finally {
    Remove-Item -LiteralPath $stderrPath -ErrorAction Stop
}

if ($actual.Count -ne $expected.Count) {
    throw "Codec reference mismatch: expected $($expected.Count) data lines, got $($actual.Count)."
}

for ($index = 0; $index -lt $expected.Count; ++$index) {
    if ($actual[$index] -cne $expected[$index]) {
        $lineNumber = $index + 1
        throw @"
Codec reference mismatch at data line $lineNumber.
Expected: $($expected[$index])
Actual:   $($actual[$index])
"@
    }
}

Write-Host "Codec reference matches exactly ($($actual.Count) movies)."
