[CmdletBinding()]
param(
    [string] $VisualStudioRoot,
    [switch] $DisableAssembly,
    [switch] $ForceRebuild,
    [switch] $VerifySourceOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Version = '8.1.2'
$SourceUrl = "https://ffmpeg.org/releases/ffmpeg-$Version.tar.xz"
$SignatureUrl = "$SourceUrl.asc"
$KeyUrl = 'https://ffmpeg.org/ffmpeg-devel.asc'
$SourceSha256 = '464BEB5E7BF0C311E68B45AE2F04E9CC2AF88851ABB4082231742A74D97B524C'
$SignatureSha256 = '0A0963FCCD70597838073F3E31B20F4A4D8CC2B5E577472C9A5A1F22624246F8'
$KeySha256 = '397B3BECEDCD5A98769967FF1FF8501DDC89F8368B8F766E4701377D7DBAABE5'
$ReleaseFingerprint = 'FCF986EA15E6E293A5644F10B4322F04D67658D8'
$BuildRecipeVersion = '4'

$DownloadRoot = Join-Path $RepoRoot 'build\downloads'
$DependencyRoot = Join-Path $RepoRoot `
    "build\dependencies\ffmpeg-$Version-msvc-x64-static"
$SourceParent = Join-Path $DependencyRoot 'source'
$SourceRoot = Join-Path $SourceParent "ffmpeg-$Version"
$BuildRoot = Join-Path $DependencyRoot 'build'
$InstallRoot = Join-Path $DependencyRoot 'install'
$MetadataPath = Join-Path $DependencyRoot 'build-metadata.json'
$ManifestPath = Join-Path $DependencyRoot 'install-manifest.json'
$SourceArchive = Join-Path $DownloadRoot "ffmpeg-$Version.tar.xz"
$Signature = "$SourceArchive.asc"
$ReleaseKey = Join-Path $DownloadRoot 'ffmpeg-devel.asc'

$ConfigureOptions = @(
    '--toolchain=msvc',
    '--arch=x86_64',
    '--target-os=win32',
    '--prefix=../install',
    '--disable-shared',
    '--enable-static',
    '--disable-programs',
    '--disable-doc',
    '--disable-network',
    '--disable-debug',
    '--disable-autodetect',
    '--disable-everything',
    '--disable-avdevice',
    '--disable-avfilter',
    '--enable-avcodec',
    '--enable-avformat',
    '--enable-avutil',
    '--enable-swresample',
    '--enable-swscale',
    '--enable-demuxer=str',
    '--enable-decoder=mdec',
    '--enable-decoder=adpcm_xa',
    '--enable-w32threads',
    '--disable-pthreads',
    '--extra-cflags=-Brepro',
    '--extra-cflags=-MT',
    '--extra-cflags=-GL'
)
if ($DisableAssembly) {
    $ConfigureOptions += '--disable-asm'
}

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

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)][string] $Value)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Value)
        return ([BitConverter]::ToString(
            $algorithm.ComputeHash($bytes))).Replace('-', '').ToUpperInvariant()
    } finally {
        $algorithm.Dispose()
    }
}

function Get-RelativePath {
    param(
        [Parameter(Mandatory = $true)][string] $BasePath,
        [Parameter(Mandatory = $true)][string] $Path
    )
    $baseUri = [Uri]::new(($BasePath.TrimEnd('\') + '\'))
    $pathUri = [Uri]::new($Path)
    return [Uri]::UnescapeDataString(
        $baseUri.MakeRelativeUri($pathUri).ToString()).Replace('/', '\')
}

function ConvertTo-MsysPath {
    param([Parameter(Mandatory = $true)][string] $Path)
    $fullPath = [IO.Path]::GetFullPath($Path).Replace('\', '/')
    if ($fullPath -notmatch '^([A-Za-z]):/(.*)$') {
        throw "Could not convert Windows path for Git GnuPG: $fullPath"
    }
    return "/$($Matches[1].ToLowerInvariant())/$($Matches[2])"
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

function Get-ConfigurationId {
    $identity = [ordered]@{
        build_recipe_version = $BuildRecipeVersion
        version = $Version
        source_sha256 = $SourceSha256
        signature_sha256 = $SignatureSha256
        key_sha256 = $KeySha256
        release_fingerprint = $ReleaseFingerprint
        configure_options = $ConfigureOptions
    }
    return Get-TextSha256 ($identity | ConvertTo-Json -Depth 5 -Compress)
}

function Get-VerifiedDownload {
    param(
        [Parameter(Mandatory = $true)][string] $Url,
        [Parameter(Mandatory = $true)][string] $Path,
        [Parameter(Mandatory = $true)][string] $ExpectedSha256
    )
    if ((Test-Path -LiteralPath $Path -PathType Leaf) -and
            (Get-Sha256 $Path) -eq $ExpectedSha256) {
        return
    }
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Force
    }
    $curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
    if (-not $curl) {
        throw 'curl.exe was not found; it is included with supported Windows versions.'
    }
    Write-Host "Downloading $Url"
    Invoke-Native $curl '--fail' '--location' '--connect-timeout' '30' `
        '--retry' '8' '--retry-all-errors' '--retry-max-time' '300' `
        '--output' $Path $Url
    $actual = Get-Sha256 $Path
    if ($actual -ne $ExpectedSha256) {
        Remove-Item -LiteralPath $Path -Force
        throw "Downloaded file checksum mismatch for $Url (received $actual)."
    }
}

function Test-ReleaseSignature {
    $gitCommand = (Get-Command git.exe -ErrorAction SilentlyContinue).Source
    $gitRoot = if ($gitCommand) {
        Split-Path -Parent (Split-Path -Parent $gitCommand)
    } else { '' }
    $gpg = if ($gitRoot -and
            (Test-Path -LiteralPath (Join-Path $gitRoot 'usr\bin\gpg.exe') `
                -PathType Leaf)) {
        Join-Path $gitRoot 'usr\bin\gpg.exe'
    } else { (Get-Command gpg.exe -ErrorAction SilentlyContinue).Source }
    $gpgv = if ($gitRoot -and
            (Test-Path -LiteralPath (Join-Path $gitRoot 'usr\bin\gpgv.exe') `
                -PathType Leaf)) {
        Join-Path $gitRoot 'usr\bin\gpgv.exe'
    } else { (Get-Command gpgv.exe -ErrorAction SilentlyContinue).Source }
    if (-not $gpg -or -not $gpgv) {
        throw 'gpg.exe and gpgv.exe were not found; install Git for Windows.'
    }
    $gpgRoot = Join-Path $DependencyRoot 'gpg-verify'
    Remove-VerifiedTree $gpgRoot $DependencyRoot
    New-Item -ItemType Directory -Force -Path $gpgRoot | Out-Null
    $useMsysPaths = $gpg -match '(?i)[\\/]usr[\\/]bin[\\/]gpg\.exe$'
    $gpgRootArgument = if ($useMsysPaths) {
        ConvertTo-MsysPath $gpgRoot
    } else { $gpgRoot }
    $releaseKeyArgument = if ($useMsysPaths) {
        ConvertTo-MsysPath $ReleaseKey
    } else { $ReleaseKey }
    $signatureArgument = if ($useMsysPaths) {
        ConvertTo-MsysPath $Signature
    } else { $Signature }
    $sourceArchiveArgument = if ($useMsysPaths) {
        ConvertTo-MsysPath $SourceArchive
    } else { $SourceArchive }
    try {
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $keyInfo = @(& $gpg --homedir $gpgRootArgument --batch --no-autostart `
                --with-colons --show-keys $releaseKeyArgument)
            $keyExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $oldPreference
        }
        if ($keyExit -ne 0) {
            throw 'Could not inspect the pinned FFmpeg release key.'
        }
        $primaryFingerprint = @($keyInfo | Where-Object {
                $_ -match '^fpr:{9}([0-9A-F]+):$'
            } | ForEach-Object { $Matches[1] } | Select-Object -First 1)
        if ($primaryFingerprint.Count -ne 1 -or
                $primaryFingerprint[0] -ne $ReleaseFingerprint) {
            throw "Unexpected FFmpeg release-key fingerprint: $($primaryFingerprint -join ', ')"
        }

        $keyring = Join-Path $gpgRoot 'ffmpeg-release.gpg'
        $keyringArgument = if ($useMsysPaths) {
            ConvertTo-MsysPath $keyring
        } else { $keyring }
        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            & $gpg --homedir $gpgRootArgument --batch --yes --no-autostart `
                --dearmor --output $keyringArgument $releaseKeyArgument
            $dearmorExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $oldPreference
        }
        if ($dearmorExit -ne 0) {
            throw 'Could not create the isolated FFmpeg verification keyring.'
        }

        $oldPreference = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $verification = @(& $gpgv --homedir $gpgRootArgument `
                --keyring 'ffmpeg-release.gpg' --status-fd=1 `
                $signatureArgument $sourceArchiveArgument)
            $verifyExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $oldPreference
        }
        if ($verifyExit -ne 0 -or -not ($verification | Where-Object {
                    $_ -eq "[GNUPG:] VALIDSIG $ReleaseFingerprint 2026-06-17 1781661995 0 4 0 1 10 00 $ReleaseFingerprint"
                })) {
            # The timestamp fields are useful evidence but can vary in textual
            # rendering between GnuPG versions; accept only an exact signer.
            $validSigner = $verification | Where-Object {
                $_ -match '^\[GNUPG:\] VALIDSIG ([0-9A-F]+) ' -and
                $Matches[1] -eq $ReleaseFingerprint
            }
            if ($verifyExit -ne 0 -or -not $validSigner) {
                throw 'The FFmpeg source archive signature is not valid for the pinned release key.'
            }
        }
        Write-Host "Verified FFmpeg $Version source signature ($ReleaseFingerprint)."
    } finally {
        if (Test-Path -LiteralPath $gpgRoot) {
            Remove-VerifiedTree $gpgRoot $DependencyRoot
        }
    }
}

function Initialize-SourceTree {
    $marker = Join-Path $SourceRoot '.stuntmaster-source-sha256'
    if ((Test-Path -LiteralPath (Join-Path $SourceRoot 'configure') -PathType Leaf) -and
            (Test-Path -LiteralPath (Join-Path $SourceRoot 'COPYING.LGPLv2.1') -PathType Leaf) -and
            (Test-Path -LiteralPath $marker -PathType Leaf) -and
            ((Get-Content -LiteralPath $marker -Raw).Trim() -eq $SourceSha256)) {
        return
    }
    Remove-VerifiedTree $SourceParent $DependencyRoot
    New-Item -ItemType Directory -Force -Path $SourceParent | Out-Null
    # Extract with MSYS2's GNU tar + xz through bash rather than the Windows
    # inbox bsdtar. The bsdtar shipped on the GitHub windows-2022 image (an
    # older libarchive/liblzma) hangs indefinitely while inventorying this
    # .tar.xz; both current local bsdtar and GNU tar handle it in under a
    # second. POSIX operands plus --force-local also remove any chance of a
    # drive-letter colon being interpreted as remote-archive syntax, and a
    # `timeout` watchdog guarantees a hang can never burn the job's wall clock.
    # Use a non-login shell with an explicit PATH: a fresh runner's MSYS2
    # first login prints "Copying skeleton files." to stdout, which would
    # otherwise be captured as a bogus archive entry. /usr/bin holds tar, xz,
    # and timeout, so no login profile is needed.
    $bash = 'C:\msys64\usr\bin\bash.exe'
    if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
        throw 'MSYS2 was not found at C:\msys64; it is required to extract FFmpeg source.'
    }
    $archiveMsys = ConvertTo-MsysPath $SourceArchive
    $sourceParentMsys = ConvertTo-MsysPath $SourceParent
    Write-Host "Inventorying verified FFmpeg archive with MSYS2 GNU tar ($archiveMsys)"
    $entries = @(& $bash -c "export PATH=/usr/bin; timeout 120 tar --force-local -tJf '$archiveMsys'" |
        Where-Object { $_.Trim() -ne '' })
    if ($LASTEXITCODE -eq 124) {
        throw 'Timed out inventorying the FFmpeg source archive (tar watchdog).'
    }
    if ($LASTEXITCODE -ne 0 -or $entries.Count -eq 0) {
        throw 'Could not inventory the FFmpeg source archive.'
    }
    $invalid = @($entries | Where-Object {
        $_ -notmatch "^ffmpeg-$([regex]::Escape($Version))(?:/|$)" -or
        $_ -match '\\' -or $_ -match '(^|/)\.\.?(?:/|$)'
    })
    if ($invalid.Count -ne 0) {
        throw "FFmpeg source archive contains unsafe paths: $($invalid[0])"
    }
    Write-Host 'Extracting verified FFmpeg source archive...'
    & $bash -c "export PATH=/usr/bin; timeout 300 tar --force-local -xJf '$archiveMsys' -C '$sourceParentMsys'"
    if ($LASTEXITCODE -eq 124) {
        throw 'Timed out extracting the FFmpeg source archive (tar watchdog).'
    }
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg source extraction failed with exit code $LASTEXITCODE."
    }
    foreach ($required in @('configure', 'Makefile', 'RELEASE')) {
        if (-not (Test-Path -LiteralPath (Join-Path $SourceRoot $required) -PathType Leaf)) {
            $count = @(Get-ChildItem -LiteralPath $SourceRoot -Recurse -File `
                -ErrorAction SilentlyContinue).Count
            throw "FFmpeg source extraction is incomplete: '$required' missing from $SourceRoot ($count files present)."
        }
    }
    [IO.File]::WriteAllText($marker, "$SourceSha256`n", [Text.UTF8Encoding]::new($false))
}

function Import-VisualStudioEnvironment {
    $vsDevCmd = Join-Path $VisualStudioRoot 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path -LiteralPath $vsDevCmd -PathType Leaf)) {
        throw "Visual Studio developer environment was not found: $vsDevCmd"
    }
    $command = 'call "{0}" -no_logo -arch=x64 -host_arch=x64 && set' -f `
        $vsDevCmd
    $environment = @(& $env:ComSpec /d /s /c $command)
    if ($LASTEXITCODE -ne 0) {
        throw 'Visual Studio developer environment initialization failed.'
    }
    $pathValue = $null
    foreach ($line in $environment) {
        if ($line -match '^([^=]+)=(.*)$') {
            if ($Matches[1] -ieq 'PATH') {
                $pathValue = $Matches[2]
            } elseif ($Matches[1] -ine 'Path') {
                [Environment]::SetEnvironmentVariable(
                    $Matches[1], $Matches[2], 'Process')
            }
        }
    }
    if (-not $pathValue) {
        throw 'Visual Studio developer environment did not return PATH.'
    }
    [Environment]::SetEnvironmentVariable('PATH', $null, 'Process')
    [Environment]::SetEnvironmentVariable('Path', $pathValue, 'Process')
}

function Get-InstallManifest {
    $records = @()
    foreach ($file in (Get-ChildItem -LiteralPath $InstallRoot -Recurse -File |
            Sort-Object FullName)) {
        $records += [ordered]@{
            path = (Get-RelativePath $InstallRoot $file.FullName).Replace('\', '/')
            sha256 = (Get-Sha256 $file.FullName).ToLowerInvariant()
        }
    }
    return @($records)
}

function Test-CachedBuild {
    $configurationId = Get-ConfigurationId
    if (-not (Test-Path -LiteralPath $MetadataPath -PathType Leaf) -or
            -not (Test-Path -LiteralPath $ManifestPath -PathType Leaf) -or
            -not (Test-Path -LiteralPath $InstallRoot -PathType Container)) {
        Write-Host 'FFmpeg cache miss: build metadata or installed files are absent.'
        return $false
    }
    try {
        $metadata = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
        $parsedManifest = Get-Content -LiteralPath $ManifestPath -Raw |
            ConvertFrom-Json
        $expectedManifest = @($parsedManifest)
    } catch {
        Write-Host 'FFmpeg cache miss: build metadata could not be parsed.'
        return $false
    }
    if ($metadata.configuration_id -ne $configurationId -or
            $metadata.source_sha256 -ne $SourceSha256.ToLowerInvariant() -or
            $metadata.install_manifest_sha256 -ne
                (Get-Sha256 $ManifestPath).ToLowerInvariant() -or
            $expectedManifest.Count -eq 0) {
        Write-Host 'FFmpeg cache miss: source or build configuration changed.'
        return $false
    }
    $actualManifest = @(Get-InstallManifest)
    if ($actualManifest.Count -ne $expectedManifest.Count) {
        Write-Host 'FFmpeg cache miss: installed file count does not match its manifest.'
        return $false
    }
    for ($index = 0; $index -lt $actualManifest.Count; ++$index) {
        if ($actualManifest[$index].path -ne $expectedManifest[$index].path -or
                $actualManifest[$index].sha256 -ne $expectedManifest[$index].sha256) {
            Write-Host "FFmpeg cache miss: installed file failed verification: $($actualManifest[$index].path)"
            return $false
        }
    }
    Write-Host "Reusing verified FFmpeg $Version static build from $InstallRoot"
    return $true
}

function Assert-MinimalConfiguration {
    $config = Get-Content -LiteralPath (Join-Path $BuildRoot 'config.h') -Raw
    foreach ($required in @(
            'CONFIG_STATIC', 'CONFIG_AVCODEC', 'CONFIG_AVFORMAT',
            'CONFIG_AVUTIL', 'CONFIG_SWRESAMPLE', 'CONFIG_SWSCALE')) {
        if ($config -notmatch "(?m)^#define $required 1$") {
            throw "Required FFmpeg configuration is absent: $required"
        }
    }
    foreach ($forbidden in @(
            'CONFIG_SHARED', 'CONFIG_NETWORK', 'CONFIG_GPL',
            'CONFIG_GPLV3', 'CONFIG_NONFREE', 'CONFIG_VERSION3',
            'CONFIG_AVDEVICE', 'CONFIG_AVFILTER')) {
        if ($config -notmatch "(?m)^#define $forbidden 0$") {
            throw "Forbidden FFmpeg configuration is enabled: $forbidden"
        }
    }
    $componentPath = Join-Path $BuildRoot 'config_components.h'
    $enabled = @(Get-Content -LiteralPath $componentPath | Where-Object {
            $_ -match '^#define CONFIG_.+_(DECODER|DEMUXER|ENCODER|MUXER|PROTOCOL|FILTER|INDEV|OUTDEV|BSF|PARSER) 1$'
        } | ForEach-Object { ($_ -split '\s+')[1] } | Sort-Object)
    $expected = @(
        'CONFIG_ADPCM_XA_DECODER',
        'CONFIG_MDEC_DECODER',
        'CONFIG_STR_DEMUXER'
    )
    if (($enabled -join "`n") -ne ($expected -join "`n")) {
        throw "Unexpected FFmpeg components are enabled: $($enabled -join ', ')"
    }
}

New-Item -ItemType Directory -Force -Path $DownloadRoot, $DependencyRoot |
    Out-Null
Get-VerifiedDownload $SourceUrl $SourceArchive $SourceSha256
Get-VerifiedDownload $SignatureUrl $Signature $SignatureSha256
Get-VerifiedDownload $KeyUrl $ReleaseKey $KeySha256
Test-ReleaseSignature

if ($VerifySourceOnly) {
    Write-Host "Verified authentic FFmpeg $Version source inputs."
    exit 0
}

if (-not $ForceRebuild -and (Test-CachedBuild)) {
    Write-Output $InstallRoot
    exit 0
}

Initialize-SourceTree
if (-not $VisualStudioRoot) {
    throw 'VisualStudioRoot is required when building FFmpeg.'
}
Import-VisualStudioEnvironment
$bash = 'C:\msys64\usr\bin\bash.exe'
if (-not (Test-Path -LiteralPath $bash -PathType Leaf)) {
    throw 'MSYS2 was not found at C:\msys64; it is required by FFmpeg configure.'
}
$env:MSYS2_PATH_TYPE = 'inherit'
if (-not $DisableAssembly) {
    & $bash -lc 'command -v nasm >/dev/null && nasm -v'
    if ($LASTEXITCODE -ne 0) {
        throw 'NASM was not found in MSYS2. Install it with: pacman -S --needed nasm'
    }
}

foreach ($path in @($BuildRoot, $InstallRoot)) {
    Remove-VerifiedTree $path $DependencyRoot
}
New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

$savedTemp = $env:TEMP
$savedTmp = $env:TMP
$savedSourceDateEpoch = $env:SOURCE_DATE_EPOCH
$nativeBuildRoot = $BuildRoot
$env:STUNTMASTER_FFMPEG_BUILD_ROOT = $nativeBuildRoot
$tempRoot = Join-Path $BuildRoot 'tmp'
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
$env:STUNTMASTER_FFMPEG_TEMP_ROOT = $tempRoot
$env:TEMP = $tempRoot
$env:TMP = $tempRoot
$releaseFile = Get-Item -LiteralPath (Join-Path $SourceRoot 'RELEASE')
$FfmpegSourceDateEpoch = [DateTimeOffset]::new(
    $releaseFile.LastWriteTimeUtc).ToUnixTimeSeconds().ToString()
$env:SOURCE_DATE_EPOCH = $FfmpegSourceDateEpoch
try {
    $arguments = $ConfigureOptions -join ' '
    Write-Host "Configuring minimal static FFmpeg $Version..."
    & $bash -lc `
        "export TEMP=`"`$STUNTMASTER_FFMPEG_TEMP_ROOT`" TMP=`"`$STUNTMASTER_FFMPEG_TEMP_ROOT`"; cd `"`$STUNTMASTER_FFMPEG_BUILD_ROOT`" && ../source/ffmpeg-$Version/configure $arguments"
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg configure failed with exit code $LASTEXITCODE."
    }
    Assert-MinimalConfiguration
    $jobs = [Math]::Max(1, [Environment]::ProcessorCount)
    & $bash -lc `
        "export TEMP=`"`$STUNTMASTER_FFMPEG_TEMP_ROOT`" TMP=`"`$STUNTMASTER_FFMPEG_TEMP_ROOT`"; cd `"`$STUNTMASTER_FFMPEG_BUILD_ROOT`" && /usr/bin/make -j$jobs && /usr/bin/make prefix=../install install-libs install-headers"
    if ($LASTEXITCODE -ne 0) {
        throw "FFmpeg build or install failed with exit code $LASTEXITCODE."
    }
} finally {
    $env:TEMP = $savedTemp
    $env:TMP = $savedTmp
    $env:SOURCE_DATE_EPOCH = $savedSourceDateEpoch
    Remove-Item Env:STUNTMASTER_FFMPEG_BUILD_ROOT -ErrorAction SilentlyContinue
    Remove-Item Env:STUNTMASTER_FFMPEG_TEMP_ROOT -ErrorAction SilentlyContinue
}

$requiredFiles = @(
    'include\libavcodec\avcodec.h',
    'include\libavformat\avformat.h',
    'include\libavutil\avutil.h',
    'include\libswresample\swresample.h',
    'include\libswscale\swscale.h',
    'lib\avcodec.lib',
    'lib\avformat.lib',
    'lib\avutil.lib',
    'lib\swresample.lib',
    'lib\swscale.lib'
)
foreach ($relative in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $InstallRoot $relative) -PathType Leaf)) {
        throw "Minimal FFmpeg install is missing $relative."
    }
}
$unexpectedBinaries = @(Get-ChildItem -LiteralPath $InstallRoot -Recurse -File |
    Where-Object { $_.Extension -in @('.dll', '.exe') })
if ($unexpectedBinaries.Count -ne 0) {
    throw "Static FFmpeg install contains unexpected runtime binaries: $($unexpectedBinaries.Name -join ', ')"
}

$licenseRoot = Join-Path $InstallRoot 'share\licenses\ffmpeg'
New-Item -ItemType Directory -Force -Path $licenseRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $SourceRoot 'COPYING.LGPLv2.1') `
    -Destination (Join-Path $licenseRoot 'COPYING.LGPLv2.1')
Copy-Item -LiteralPath (Join-Path $SourceRoot 'COPYING.LGPLv3') `
    -Destination (Join-Path $licenseRoot 'COPYING.LGPLv3')
Copy-Item -LiteralPath (Join-Path $SourceRoot 'LICENSE.md') `
    -Destination (Join-Path $licenseRoot 'LICENSE.md')
$evidenceRoot = Join-Path $InstallRoot 'share\ffmpeg-build'
New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $BuildRoot 'config.h') `
    -Destination (Join-Path $evidenceRoot 'config.h')
Copy-Item -LiteralPath (Join-Path $BuildRoot 'config_components.h') `
    -Destination (Join-Path $evidenceRoot 'config_components.h')
Copy-Item -LiteralPath (Join-Path $BuildRoot 'ffbuild\config.mak') `
    -Destination (Join-Path $evidenceRoot 'config.mak')
[IO.File]::WriteAllText(
    (Join-Path $evidenceRoot 'configure-options.txt'),
    (($ConfigureOptions -join "`n") + "`n"),
    [Text.UTF8Encoding]::new($false))

$manifest = @(Get-InstallManifest)
[IO.File]::WriteAllText(
    $ManifestPath,
    ($manifest | ConvertTo-Json -Depth 4),
    [Text.UTF8Encoding]::new($false))
$metadata = [ordered]@{
    schema_version = '1'
    build_recipe_version = $BuildRecipeVersion
    ffmpeg_version = $Version
    source_url = $SourceUrl
    source_sha256 = $SourceSha256.ToLowerInvariant()
    signature_url = $SignatureUrl
    signature_sha256 = $SignatureSha256.ToLowerInvariant()
    release_key_url = $KeyUrl
    release_key_sha256 = $KeySha256.ToLowerInvariant()
    release_key_fingerprint = $ReleaseFingerprint
    configuration_id = Get-ConfigurationId
    configure_options = $ConfigureOptions
    source_date_epoch = $FfmpegSourceDateEpoch
    install_manifest_sha256 = (Get-Sha256 $ManifestPath).ToLowerInvariant()
}
[IO.File]::WriteAllText(
    $MetadataPath,
    ($metadata | ConvertTo-Json -Depth 5),
    [Text.UTF8Encoding]::new($false))

Write-Host "Built and verified minimal static FFmpeg ${Version}: $InstallRoot"
Write-Output $InstallRoot
