[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $Archive,
    [Parameter(Mandatory = $true)]
    [string] $OutputPath,
    [string] $VcpkgStatusPath = 'build/windows/vcpkg_installed/vcpkg/status',
    [string] $SourceCommit = '',
    [string] $SourceRepository = 'neonoxd/psyx-stuntmaster',
    [string] $DocumentNamespace = '',
    [string] $BuildEnvironmentPath = '',
    [string] $SummaryPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$Archive = (Resolve-Path $Archive).Path

function Get-AbsolutePath {
    param([Parameter(Mandatory = $true)][string] $Path)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Path))
}

$OutputPath = Get-AbsolutePath $OutputPath
$VcpkgStatusPath = Get-AbsolutePath $VcpkgStatusPath
if ($SummaryPath) {
    $SummaryPath = Get-AbsolutePath $SummaryPath
}
if ($BuildEnvironmentPath) {
    $BuildEnvironmentPath = Get-AbsolutePath $BuildEnvironmentPath
}
if ($SourceRepository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    throw "Invalid GitHub source repository: $SourceRepository"
}

function Get-HexDigest {
    param(
        [Parameter(Mandatory = $true)]
        [System.IO.Stream] $Stream,
        [Parameter(Mandatory = $true)]
        [System.Security.Cryptography.HashAlgorithm] $Algorithm
    )
    try {
        return ([System.BitConverter]::ToString(
            $Algorithm.ComputeHash($Stream))).Replace('-', '').ToLowerInvariant()
    } finally {
        $Algorithm.Dispose()
        $Stream.Dispose()
    }
}

function Get-TextSha256 {
    param([Parameter(Mandatory = $true)][string] $Value)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Value)
        return ([BitConverter]::ToString(
            $algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $algorithm.Dispose()
    }
}

function Read-VcpkgStatus {
    param([Parameter(Mandatory = $true)][string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "vcpkg status file was not found: $Path"
    }
    $packages = @()
    foreach ($paragraph in ((Get-Content -LiteralPath $Path -Raw) -split `
            '(?:\r?\n){2,}')) {
        $fields = @{}
        foreach ($line in ($paragraph -split '\r?\n')) {
            if ($line -match '^([^:]+):\s*(.*)$') {
                $fields[$Matches[1]] = $Matches[2]
            }
        }
        if ($fields.ContainsKey('Package') -and
                $fields.ContainsKey('Version') -and
                $fields.ContainsKey('Architecture')) {
            $packages += [pscustomobject]@{
                Name = [string]$fields['Package']
                Version = [string]$fields['Version']
                PortVersion = $(if ($fields.ContainsKey('Port-Version')) {
                    [string]$fields['Port-Version']
                } else { '0' })
                Architecture = [string]$fields['Architecture']
            }
        }
    }
    return @($packages | Sort-Object Name, Architecture -Unique)
}

function New-Package {
    param(
        [Parameter(Mandatory = $true)][string] $Id,
        [Parameter(Mandatory = $true)][string] $Name,
        [Parameter(Mandatory = $true)][string] $Version,
        [Parameter(Mandatory = $true)][string] $DownloadLocation,
        [Parameter(Mandatory = $true)][string] $License,
        [Parameter(Mandatory = $true)][bool] $FilesAnalyzed,
        [string] $LicenseConcluded = '',
        [string] $Purl = '',
        [string] $Comment = ''
    )
    $concludedLicense = if ($LicenseConcluded) {
        $LicenseConcluded
    } else { $License }
    $package = [ordered]@{
        SPDXID = $Id
        name = $Name
        versionInfo = $Version
        downloadLocation = $DownloadLocation
        filesAnalyzed = $FilesAnalyzed
        licenseConcluded = $concludedLicense
        licenseDeclared = $License
        copyrightText = 'NOASSERTION'
    }
    if ($Purl) {
        $package.externalRefs = @([ordered]@{
            referenceCategory = 'PACKAGE-MANAGER'
            referenceType = 'purl'
            referenceLocator = $Purl
        })
    }
    if ($Comment) {
        $package.comment = $Comment
    }
    return $package
}

$cmake = Get-Content -LiteralPath (Join-Path $RepoRoot 'CMakeLists.txt') -Raw
if ($cmake -notmatch 'project\(stuntmaster_pc\s+VERSION\s+([^\s\)]+)') {
    throw 'Could not determine the project version from CMakeLists.txt.'
}
$ProjectVersion = $Matches[1]

$manifest = Get-Content -LiteralPath (Join-Path $RepoRoot 'vcpkg.json') -Raw |
    ConvertFrom-Json
if ($manifest.'version-string' -ne $ProjectVersion) {
    throw "CMake version $ProjectVersion does not match vcpkg version $($manifest.'version-string')."
}

if (-not $SourceCommit) {
    $SourceCommit = (& git -C $RepoRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not determine the source commit.'
    }
}
$sourceTimestamp = (& git -C $RepoRoot show -s --format=%cI $SourceCommit).Trim()
if ($LASTEXITCODE -ne 0 -or -not $sourceTimestamp) {
    throw "Could not determine the source timestamp for $SourceCommit."
}
$sourceCreationTime = [DateTimeOffset]::Parse(
    $sourceTimestamp,
    [Globalization.CultureInfo]::InvariantCulture,
    [Globalization.DateTimeStyles]::RoundtripKind).UtcDateTime.ToString(
        'yyyy-MM-ddTHH:mm:ssZ')

$buildEnvironment = $null
if ($BuildEnvironmentPath) {
    if (-not (Test-Path -LiteralPath $BuildEnvironmentPath -PathType Leaf)) {
        throw "Build environment file was not found: $BuildEnvironmentPath"
    }
    $buildEnvironment = Get-Content -LiteralPath $BuildEnvironmentPath -Raw |
        ConvertFrom-Json
    $requiredEnvironmentFields = @(
        'schema_version',
        'build_system',
        'runner_image',
        'runner_image_os',
        'runner_image_version',
        'runner_architecture',
        'visual_studio_version',
        'msvc_compiler_id',
        'msvc_compiler_version',
        'msvc_toolset',
        'msvc_toolset_version',
        'windows_sdk_version',
        'cmake_version',
        'cmake_generator',
        'cmake_generator_platform',
        'git_version',
        'build_configuration',
        'vcpkg_triplet',
        'vcpkg_builtin_baseline',
        'ffmpeg_version',
        'ffmpeg_source_url',
        'ffmpeg_source_sha256',
        'ffmpeg_signature_sha256',
        'ffmpeg_release_key_fingerprint',
        'ffmpeg_configuration_id',
        'ffmpeg_linkage',
        'ffmpeg_configure_options',
        'nasm_version',
        'source_date_epoch',
        'reproducibility_options',
        'toolchain_digest_algorithm',
        'toolchain_digest_format',
        'toolchain_digest',
        'toolchain_tools'
    )
    foreach ($field in $requiredEnvironmentFields) {
        $property = $buildEnvironment.PSObject.Properties[$field]
        if ($null -eq $property -or $null -eq $property.Value -or
                ($property.Value -is [string] -and -not $property.Value) -or
                ($property.Value -is [Array] -and $property.Value.Count -eq 0)) {
            throw "Build environment field is missing or empty: $field"
        }
    }
    if ($buildEnvironment.schema_version -ne '3') {
        throw "Unsupported build environment schema: $($buildEnvironment.schema_version)"
    }
    if ($buildEnvironment.ffmpeg_version -ne '8.1.2' -or
            $buildEnvironment.ffmpeg_source_sha256 -ne
                '464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c' -or
            $buildEnvironment.ffmpeg_linkage -ne 'static') {
        throw 'Build environment does not describe the pinned static FFmpeg build.'
    }
    if ($buildEnvironment.toolchain_digest_algorithm -ne 'SHA256') {
        throw "Unsupported toolchain digest algorithm: $($buildEnvironment.toolchain_digest_algorithm)"
    }
    if ($buildEnvironment.toolchain_digest -notmatch '^[0-9a-f]{64}$') {
        throw 'The toolchain digest is not a lowercase SHA-256 value.'
    }
    $requiredToolFields = @(
        'name',
        'file_name',
        'sha256',
        'authenticode_status',
        'signature_required',
        'signer_subject',
        'signer_certificate_sha1',
        'timestamp_signer_subject',
        'timestamp_certificate_sha1'
    )
    $toolNames = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    foreach ($tool in @($buildEnvironment.toolchain_tools)) {
        foreach ($field in $requiredToolFields) {
            $property = $tool.PSObject.Properties[$field]
            if ($null -eq $property -or $null -eq $property.Value -or
                    ($property.Value -is [string] -and -not $property.Value)) {
                throw "Build tool field is missing or empty: $field"
            }
        }
        if (-not $toolNames.Add([string]$tool.name)) {
            throw "Duplicate build tool name: $($tool.name)"
        }
        if ($tool.sha256 -notmatch '^[0-9a-f]{64}$') {
            throw "Invalid SHA-256 for build tool: $($tool.name)"
        }
        if ($tool.authenticode_status -notin @('Valid', 'NotSigned')) {
            throw "Invalid Authenticode result for build tool $($tool.name): $($tool.authenticode_status)"
        }
        if ([bool]$tool.signature_required -and
                $tool.authenticode_status -ne 'Valid') {
            throw "A required build-tool signature is not valid: $($tool.name)"
        }
    }
    if ($toolNames.Count -eq 0) {
        throw 'The build environment has no tool integrity records.'
    }
    $toolchainJson = $buildEnvironment.toolchain_tools |
        ConvertTo-Json -Depth 6 -Compress
    $actualToolchainDigest = Get-TextSha256 $toolchainJson
    if ($actualToolchainDigest -ne $buildEnvironment.toolchain_digest) {
        throw "Toolchain digest mismatch: expected $($buildEnvironment.toolchain_digest), calculated $actualToolchainDigest"
    }
}
$PsyCrossCommit = (& git -C $RepoRoot rev-parse HEAD:external/PsyCross).Trim()
if ($LASTEXITCODE -ne 0) {
    throw 'Could not determine the PsyCross gitlink.'
}

$archiveStream = [IO.File]::OpenRead($Archive)
$archiveSha256 = Get-HexDigest -Stream $archiveStream `
    -Algorithm ([Security.Cryptography.SHA256]::Create())
if (-not $DocumentNamespace) {
    $DocumentNamespace =
        "urn:stuntmaster-pc:sbom:${SourceCommit}:${archiveSha256}"
}

$licenseByPort = @{
    'fmt' = 'MIT'
    'openal-soft' = 'LGPL-2.0-or-later'
    'sdl2' = 'Zlib'
}
$sourceByPort = @{
    'fmt' = 'https://github.com/fmtlib/fmt'
    'openal-soft' = 'https://github.com/kcat/openal-soft'
    'sdl2' = 'https://github.com/libsdl-org/SDL'
}

$packages = @()
$packages += New-Package `
    -Id 'SPDXRef-Package-Stuntmaster' `
    -Name 'stuntmaster-pc' `
    -Version $ProjectVersion `
    -DownloadLocation "https://github.com/$SourceRepository/tree/$SourceCommit" `
    -License 'MIT' `
    -FilesAnalyzed $true `
    -LicenseConcluded 'NOASSERTION' `
    -Purl "pkg:github/$SourceRepository@$SourceCommit" `
    -Comment 'The project declares MIT; no single license is concluded for the complete Windows distribution because it also contains third-party runtime and license files.'
$packages += New-Package `
    -Id 'SPDXRef-Package-PsyCross' `
    -Name 'PsyCross' `
    -Version $PsyCrossCommit `
    -DownloadLocation "https://github.com/neonoxd/PsyCross/tree/$PsyCrossCommit" `
    -License 'MIT' `
    -FilesAnalyzed $false `
    -Purl "pkg:github/neonoxd/PsyCross@$PsyCrossCommit" `
    -Comment 'Statically linked into stuntmaster.exe.'
$ffmpeg = New-Package `
    -Id 'SPDXRef-Package-FFmpeg-LGPL' `
    -Name 'FFmpeg' `
    -Version '8.1.2' `
    -DownloadLocation 'https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz' `
    -License 'LGPL-2.1-or-later' `
    -FilesAnalyzed $false `
    -Purl 'pkg:generic/ffmpeg@8.1.2' `
    -Comment 'Built from the authentic signed upstream source and statically linked into stuntmaster.exe. The minimal build enables only the str demuxer plus mdec and adpcm_xa decoders; no GPL, nonfree, network, program, encoder, muxer, protocol, filter, or device component is enabled.'
$ffmpeg.checksums = @([ordered]@{
    algorithm = 'SHA256'
    checksumValue = '464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c'
})
$packages += $ffmpeg

$vcpkgPackages = Read-VcpkgStatus -Path $VcpkgStatusPath
$vcpkgPackageIds = @()
$vcpkgRuntimePackageIds = @{}
foreach ($dependency in $vcpkgPackages) {
    $safeName = $dependency.Name -replace '[^A-Za-z0-9.-]', '-'
    $safeArchitecture = $dependency.Architecture -replace '[^A-Za-z0-9.-]', '-'
    $id = "SPDXRef-Package-vcpkg-$safeName-$safeArchitecture"
    $vcpkgPackageIds += $id
    if ($dependency.Architecture -eq 'x64-windows-static-release' -and
            $dependency.Name -in @('fmt', 'openal-soft', 'sdl2')) {
        $vcpkgRuntimePackageIds[$dependency.Name] = $id
    }
    $license = if ($licenseByPort.ContainsKey($dependency.Name)) {
        $licenseByPort[$dependency.Name]
    } else { 'NOASSERTION' }
    $source = if ($sourceByPort.ContainsKey($dependency.Name)) {
        $sourceByPort[$dependency.Name]
    } else { 'NOASSERTION' }
    $comment = "vcpkg architecture $($dependency.Architecture); port-version $($dependency.PortVersion); registry baseline $($manifest.'builtin-baseline')."
    $packages += New-Package `
        -Id $id `
        -Name $dependency.Name `
        -Version $dependency.Version `
        -DownloadLocation $source `
        -License $license `
        -FilesAnalyzed $false `
        -Purl "pkg:generic/$($dependency.Name)@$($dependency.Version)?vcpkg_triplet=$($dependency.Architecture)" `
        -Comment $comment
}
foreach ($requiredRuntimePackage in @('fmt', 'openal-soft', 'sdl2')) {
    if (-not $vcpkgRuntimePackageIds.ContainsKey($requiredRuntimePackage)) {
        throw "Required target-triplet package is missing from vcpkg status: $requiredRuntimePackage"
    }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($Archive)
$files = @()
$relationships = @(
    [ordered]@{
        spdxElementId = 'SPDXRef-DOCUMENT'
        relationshipType = 'DESCRIBES'
        relatedSpdxElement = 'SPDXRef-Package-Stuntmaster'
    },
    [ordered]@{
        spdxElementId = 'SPDXRef-Package-Stuntmaster'
        relationshipType = 'DEPENDS_ON'
        relatedSpdxElement = 'SPDXRef-Package-PsyCross'
    },
    [ordered]@{
        spdxElementId = 'SPDXRef-Package-Stuntmaster'
        relationshipType = 'DEPENDS_ON'
        relatedSpdxElement = 'SPDXRef-Package-FFmpeg-LGPL'
    }
)
foreach ($id in $vcpkgPackageIds) {
    if ($id -match '^SPDXRef-Package-vcpkg-(vcpkg-cmake|vcpkg-cmake-config)-') {
        $relationships += [ordered]@{
            spdxElementId = $id
            relationshipType = 'BUILD_DEPENDENCY_OF'
            relatedSpdxElement = 'SPDXRef-Package-Stuntmaster'
        }
    } else {
        $relationships += [ordered]@{
            spdxElementId = 'SPDXRef-Package-Stuntmaster'
            relationshipType = 'DEPENDS_ON'
            relatedSpdxElement = $id
        }
    }
}

$archiveFileSha1 = @()
try {
    $index = 0
    foreach ($entry in ($zip.Entries | Where-Object { $_.Name } |
            Sort-Object FullName)) {
        ++$index
        $sha1 = Get-HexDigest -Stream $entry.Open() `
            -Algorithm ([System.Security.Cryptography.SHA1]::Create())
        $sha256 = Get-HexDigest -Stream $entry.Open() `
            -Algorithm ([System.Security.Cryptography.SHA256]::Create())
        $fileId = 'SPDXRef-File-{0:D4}' -f $index
        $normalizedName = $entry.FullName.Replace('\', '/')
        $files += [ordered]@{
            SPDXID = $fileId
            fileName = "./$normalizedName"
            checksums = @(
                [ordered]@{ algorithm = 'SHA1'; checksumValue = $sha1 },
                [ordered]@{ algorithm = 'SHA256'; checksumValue = $sha256 }
            )
            licenseConcluded = 'NOASSERTION'
            licenseInfoInFiles = @('NOASSERTION')
            copyrightText = 'NOASSERTION'
        }
        $isFfmpeg = $normalizedName -match `
            '(?i)(^|/)licenses/FFmpeg-(LGPL-[^/]+\.txt|LICENSE\.md)$'
        $origin = if ($isFfmpeg) {
            'SPDXRef-Package-FFmpeg-LGPL'
        } elseif ($normalizedName -match '(?i)(^|/)licenses/fmt\.txt$') {
            $vcpkgRuntimePackageIds['fmt']
        } elseif ($normalizedName -match '(?i)(^|/)licenses/OpenAL-Soft\.txt$') {
            $vcpkgRuntimePackageIds['openal-soft']
        } elseif ($normalizedName -match '(?i)(^|/)licenses/SDL2\.txt$') {
            $vcpkgRuntimePackageIds['sdl2']
        } else {
            $null
        }
        $archiveFileSha1 += $sha1
        $relationships += [ordered]@{
            spdxElementId = 'SPDXRef-Package-Stuntmaster'
            relationshipType = 'CONTAINS'
            relatedSpdxElement = $fileId
        }
        if ($origin) {
            $relationships += [ordered]@{
                spdxElementId = $fileId
                relationshipType = 'GENERATED_FROM'
                relatedSpdxElement = $origin
            }
        }
    }
} finally {
    $zip.Dispose()
}

function Get-VerificationCode {
    param([Parameter(Mandatory = $true)][string[]] $Hashes)
    $bytes = [Text.Encoding]::ASCII.GetBytes(
        (($Hashes | Sort-Object) -join ''))
    $stream = [IO.MemoryStream]::new($bytes, $false)
    return Get-HexDigest -Stream $stream `
        -Algorithm ([Security.Cryptography.SHA1]::Create())
}

($packages | Where-Object { $_.SPDXID -eq 'SPDXRef-Package-Stuntmaster' }).packageVerificationCode =
    [ordered]@{ packageVerificationCodeValue = Get-VerificationCode $archiveFileSha1 }

$document = [ordered]@{
    spdxVersion = 'SPDX-2.3'
    dataLicense = 'CC0-1.0'
    SPDXID = 'SPDXRef-DOCUMENT'
    name = "stuntmaster-pc-$ProjectVersion-windows-x64"
    documentNamespace = $DocumentNamespace
    creationInfo = [ordered]@{
        created = $sourceCreationTime
        creators = @('Tool: stuntmaster-generate-sbom.ps1-1.4')
        comment = "Generated from $([IO.Path]::GetFileName($Archive)); source commit $SourceCommit."
    }
    packages = $packages
    files = $files
    relationships = $relationships
}
if ($null -ne $buildEnvironment) {
    $environmentJson = $buildEnvironment | ConvertTo-Json -Depth 6 -Compress
    $document['annotations'] = @([ordered]@{
        annotationDate = $sourceCreationTime
        annotationType = 'OTHER'
        annotator = 'Tool: stuntmaster-generate-sbom.ps1-1.4'
        comment = "Deterministic build environment metadata (JSON): $environmentJson"
    })
}

$parent = Split-Path -Parent $OutputPath
if ($parent) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
$json = $document | ConvertTo-Json -Depth 20
[IO.File]::WriteAllText($OutputPath, $json, [Text.UTF8Encoding]::new($false))

if ($SummaryPath) {
    function ConvertTo-MarkdownCell {
        param([AllowEmptyString()][string] $Value)
        return $Value.Replace('|', '\|').Replace("`r`n", '<br>').Replace(
            "`n", '<br>')
    }

    function Get-PackageRole {
        param([Parameter(Mandatory = $true)][string] $Id)
        switch ($Id) {
            'SPDXRef-Package-Stuntmaster' { return 'Application' }
            'SPDXRef-Package-PsyCross' { return 'Statically linked source dependency' }
            'SPDXRef-Package-FFmpeg-LGPL' { return 'Statically linked source dependency' }
            { $_ -match '^SPDXRef-Package-vcpkg-(vcpkg-cmake|vcpkg-cmake-config)-' } {
                return 'Build helper'
            }
            default { return 'Statically linked dependency' }
        }
    }

    $packageById = @{}
    foreach ($package in $packages) {
        $packageById[[string]$package.SPDXID] = $package
    }
    $fileComponentById = @{}
    foreach ($relationship in $relationships) {
        if ($relationship.relationshipType -eq 'GENERATED_FROM' -and
                $packageById.ContainsKey([string]$relationship.relatedSpdxElement)) {
            $fileComponentById[[string]$relationship.spdxElementId] =
                [string]$relationship.relatedSpdxElement
        }
    }

    $jsonStream = [IO.File]::OpenRead($OutputPath)
    $jsonSha256 = Get-HexDigest -Stream $jsonStream `
        -Algorithm ([Security.Cryptography.SHA256]::Create())
    $archiveName = [IO.Path]::GetFileName($Archive)
    $jsonName = [IO.Path]::GetFileName($OutputPath)
    $summaryLines = [Collections.Generic.List[string]]::new()
    $summaryLines.Add("# Software bill of materials - stuntmaster-pc $ProjectVersion")
    $summaryLines.Add('')
    $summaryLines.Add('> Human-readable companion to the authoritative SPDX 2.3 JSON document. This Markdown file is not an additional SPDX serialization.')
    $summaryLines.Add('')
    $summaryLines.Add('## Release identity')
    $summaryLines.Add('')
    $summaryLines.Add('| Field | Value |')
    $summaryLines.Add('|---|---|')
    $summaryLines.Add(('| Release archive | `{0}` |' -f $archiveName))
    $summaryLines.Add(('| Archive SHA-256 | `{0}` |' -f $archiveSha256))
    $summaryLines.Add(('| Authoritative SBOM | `{0}` |' -f $jsonName))
    $summaryLines.Add(('| SBOM SHA-256 | `{0}` |' -f $jsonSha256))
    $summaryLines.Add(('| Source repository | [`{0}`](https://github.com/{0}) |' -f $SourceRepository))
    $summaryLines.Add(('| Source commit | [`{0}`](https://github.com/{1}/tree/{0}) |' -f $SourceCommit, $SourceRepository))
    $summaryLines.Add(('| PsyCross commit | [`{0}`](https://github.com/neonoxd/PsyCross/tree/{0}) |' -f $PsyCrossCommit))
    $summaryLines.Add(('| Reproducible timestamp | `{0}` |' -f $sourceCreationTime))
    $summaryLines.Add("| Scope | $($packages.Count) packages; $($files.Count) shipped files |")
    $summaryLines.Add('')
    if ($null -ne $buildEnvironment) {
        $environmentLabels = [ordered]@{
            build_system = 'Build system'
            runner_image = 'Runner image'
            runner_image_os = 'Runner image OS'
            runner_image_version = 'Runner image version'
            runner_architecture = 'Runner architecture'
            visual_studio_version = 'Visual Studio'
            msvc_compiler_id = 'Compiler'
            msvc_compiler_version = 'Compiler version'
            msvc_toolset = 'MSVC toolset'
            msvc_toolset_version = 'MSVC tools version'
            windows_sdk_version = 'Windows SDK'
            cmake_version = 'CMake'
            cmake_generator = 'CMake generator'
            cmake_generator_platform = 'Generator platform'
            git_version = 'Git'
            build_configuration = 'Build configuration'
            vcpkg_triplet = 'vcpkg triplet'
            vcpkg_builtin_baseline = 'vcpkg baseline'
            ffmpeg_version = 'FFmpeg version'
            ffmpeg_source_url = 'FFmpeg source'
            ffmpeg_source_sha256 = 'FFmpeg source SHA-256'
            ffmpeg_signature_sha256 = 'FFmpeg signature SHA-256'
            ffmpeg_release_key_fingerprint = 'FFmpeg signing-key fingerprint'
            ffmpeg_configuration_id = 'FFmpeg configuration digest'
            ffmpeg_linkage = 'FFmpeg linkage'
            ffmpeg_configure_options = 'FFmpeg configure options'
            nasm_version = 'NASM version'
            source_date_epoch = 'SOURCE_DATE_EPOCH'
            reproducibility_options = 'Reproducibility options'
            toolchain_digest_algorithm = 'Toolchain digest algorithm'
            toolchain_digest = 'Aggregate toolchain digest'
        }
        $summaryLines.Add('## Build environment')
        $summaryLines.Add('')
        $summaryLines.Add('| Field | Value |')
        $summaryLines.Add('|---|---|')
        foreach ($entry in $environmentLabels.GetEnumerator()) {
            $rawValue = $buildEnvironment.($entry.Key)
            $value = if ($rawValue -is [Array]) {
                @($rawValue) -join ' '
            } else { [string]$rawValue }
            $summaryLines.Add(('| {0} | `{1}` |' -f
                    $entry.Value, (ConvertTo-MarkdownCell $value)))
        }
        $summaryLines.Add('')
        $summaryLines.Add('Run identifiers, workspace paths, and other incidental per-run values are intentionally omitted so identical environments can produce identical SBOMs.')
        $summaryLines.Add('')
        $summaryLines.Add('## Build tool integrity')
        $summaryLines.Add('')
        $summaryLines.Add('| Tool | File | Executable SHA-256 | Authenticode | Signer | Certificate SHA-1 |')
        $summaryLines.Add('|---|---|---|---|---|---|')
        foreach ($tool in @($buildEnvironment.toolchain_tools)) {
            $requirement = if ([bool]$tool.signature_required) {
                "$($tool.authenticode_status) (required)"
            } else { [string]$tool.authenticode_status }
            $summaryLines.Add(('| {0} | `{1}` | `{2}` | {3} | {4} | `{5}` |' -f
                    (ConvertTo-MarkdownCell ([string]$tool.name)),
                    (ConvertTo-MarkdownCell ([string]$tool.file_name)),
                    $tool.sha256,
                    (ConvertTo-MarkdownCell $requirement),
                    (ConvertTo-MarkdownCell ([string]$tool.signer_subject)),
                    $tool.signer_certificate_sha1))
        }
        $summaryLines.Add('')
        $summaryLines.Add('The aggregate digest is SHA-256 over the UTF-8 compact JSON `toolchain_tools` array in its listed and property order. Microsoft compiler, linker, librarian, MSBuild, and Windows SDK tools must have a valid Microsoft Authenticode signature; invalid signatures always fail the build. Other tools are still byte-hashed and explicitly report whether they are signed.')
        $summaryLines.Add('')
    }
    $summaryLines.Add('## Packages')
    $summaryLines.Add('')
    $summaryLines.Add('| Package | Version | Role | Declared license | Concluded license | Source |')
    $summaryLines.Add('|---|---|---|---|---|---|')
    foreach ($package in $packages) {
        $name = ConvertTo-MarkdownCell ([string]$package.name)
        $version = ConvertTo-MarkdownCell ([string]$package.versionInfo)
        $role = ConvertTo-MarkdownCell (Get-PackageRole ([string]$package.SPDXID))
        $declaredLicense = ConvertTo-MarkdownCell ([string]$package.licenseDeclared)
        $concludedLicense = ConvertTo-MarkdownCell ([string]$package.licenseConcluded)
        $location = [string]$package.downloadLocation
        $source = if ($location -match '^https?://') {
            "[source]($location)"
        } else {
            ConvertTo-MarkdownCell $location
        }
        $summaryLines.Add(
            ('| {0} | `{1}` | {2} | `{3}` | `{4}` | {5} |' -f
                $name, $version, $role, $declaredLicense,
                $concludedLicense, $source))
    }
    $summaryLines.Add('')
    $summaryLines.Add('## Shipped files')
    $summaryLines.Add('')
    $summaryLines.Add('| File | Component | SHA-256 |')
    $summaryLines.Add('|---|---|---|')
    foreach ($file in $files) {
        $componentId = if ($fileComponentById.ContainsKey([string]$file.SPDXID)) {
            [string]$fileComponentById[[string]$file.SPDXID]
        } else { 'SPDXRef-Package-Stuntmaster' }
        $componentName = if ($packageById.ContainsKey($componentId)) {
            [string]$packageById[$componentId].name
        } else { 'NOASSERTION' }
        $sha256 = [string](
            $file.checksums | Where-Object algorithm -eq 'SHA256' |
                Select-Object -First 1).checksumValue
        $fileName = ([string]$file.fileName) -replace '^\./', ''
        $summaryLines.Add(('| `{0}` | {1} | `{2}` |' -f
                (ConvertTo-MarkdownCell $fileName),
                (ConvertTo-MarkdownCell $componentName),
                $sha256))
    }
    $summaryLines.Add('')
    $summaryLines.Add('## Reading this report')
    $summaryLines.Add('')
    $summaryLines.Add('- `NOASSERTION` means the build did not infer a value; it does not mean that no license or copyright applies.')
    $summaryLines.Add('- The project declares MIT, while the complete analyzed distribution has `licenseConcluded: NOASSERTION` because it also contains separately licensed third-party files listed below.')
    $summaryLines.Add('- The file list describes the contents of the release ZIP. The SBOM and this report are intentionally published beside the ZIP rather than embedded in it.')
    $summaryLines.Add('- The analyzed `stuntmaster-pc` distributable contains every shipped file. `GENERATED_FROM` relationships identify copied dependency license files. Statically linked library bytes are represented by package dependency relationships because they are not separable files in the ZIP.')
    $summaryLines.Add('- The build environment and tool-integrity tables record non-shipped toolchain provenance. Those tools are not represented as packages or runtime dependencies in this artifact-scoped SBOM.')
    $summaryLines.Add('- Trusted non-pull-request CI runs attest this Markdown file and the other published release files as SLSA provenance subjects. The release ZIP additionally receives an SPDX SBOM attestation whose signed predicate contains the complete structured environment record. Pull-request and local outputs are not attested by this generator itself.')
    $summaryLines.Add('- For automated analysis, validation, conversion, or policy checks, use the authoritative SPDX JSON document.')

    $summaryParent = Split-Path -Parent $SummaryPath
    if ($summaryParent) {
        New-Item -ItemType Directory -Force -Path $summaryParent | Out-Null
    }
    [IO.File]::WriteAllText(
        $SummaryPath,
        (($summaryLines -join "`n") + "`n"),
        [Text.UTF8Encoding]::new($false))
    Write-Host "Human-readable SBOM: $SummaryPath"
}
Write-Host "SPDX SBOM: $OutputPath"
Write-Host "Packages: $($packages.Count); files: $($files.Count)"
