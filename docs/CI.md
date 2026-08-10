# Continuous integration and build artifacts

`.github/workflows/windows-build.yml` builds the full Windows release on a
GitHub-hosted Windows Server 2022 runner. The job checks out the pinned
submodule, records the toolchain, runs the existing build and test scripts,
packages the result, inventories its contents, generates checksums and an SPDX
SBOM, and uploads the files as a GitHub Actions artifact.

## Triggers

The workflow runs for:

- every branch push whose revision contains the workflow;
- the default `pull_request` activity types (`opened`, `synchronize`, and
  `reopened`) once the workflow exists on the target repository's default
  branch; and
- a manual `workflow_dispatch` request once the workflow exists on the
  repository's default branch.

It deliberately does not use `pull_request_target`. Pull-request builds get a
read-only token, and checkout credentials are not persisted. Concurrent runs
of the same workflow and Git ref cancel the older run. A failed build, test,
package check, positive archive-layout inspection, or SBOM generation prevents
publication. The archive policy permits only the installed executables,
configuration and project documents, the five expected FFmpeg runtime DLL
families, Markdown under `docs/`, and the four expected license files.

Every successful run uploads a GitHub Actions artifact named with the project
version and source commit. It is retained for 30 days and contains the release
ZIP, its checksum and inventory, the authoritative SPDX JSON, a generated
Markdown SBOM report for human review, and a checksum manifest covering both
SBOM representations.

After a successful branch-push or manual build, a separate Linux job downloads
that completed artifact, rechecks the exact six-file allowlist and both checksum
manifests, and asks GitHub's Sigstore-backed attestation service to create two
signed statements. The first binds the release ZIP digest to the SPDX document.
The second records all six published files as subjects of a SLSA provenance
attestation, so the human-readable report, checksums, and inventory are covered
as well as the ZIP and canonical SBOM. The attestation job does not check out or
execute repository source. Its OIDC and attestation-write permissions are
isolated from the Windows compiler job.

Pull-request builds deliberately skip attestation. In particular, code from an
external fork can compile and test with a read-only token but cannot ask the
repository to make a signed claim about its output.

`.github/workflows/draft-release.yml` runs only for tag pushes. It does not
compile the project again. Instead, it finds an unexpired successful Windows
build artifact from a non-pull-request run whose recorded source commit is
exactly the tagged commit, downloads it, verifies its file set and checksums,
requires the ZIP's verified signed SPDX predicate to equal the published JSON
sidecar, and verifies every file's SLSA provenance attestation before creating
a draft GitHub Release for the existing tag.
Verification pins the expected repository, signer workflow, source commit,
predicate type, and GitHub-hosted runner. If a matching build is still running,
the release workflow waits for up to 30 minutes. This promotes the bytes that
were already tested and attested instead of trusting a second build merely
because it used the same source revision.

If no matching artifact exists or the 30-day artifact has expired, manually run
the Windows build workflow with the tag selected as its ref, then rerun the
failed draft-release workflow. This trusted manual build also creates the
required attestation. The draft's title is the tag text, and its initial notes
are an explicit placeholder for maintainer-written release notes. A maintainer
must review and explicitly publish the draft in GitHub. Branch pushes, pull
requests, and manual workflow runs never create releases.

The release job has `contents: write` only because creating a release requires
it; the build job and all non-tag runs retain `contents: read`. The job refuses
to invent a missing tag, replace an existing release, overwrite assets, or
publish a draft automatically.

Consumers can verify a downloaded release archive and recover the signed SPDX
predicate with GitHub CLI:

```console
TAG_COMMIT_SHA=replace-with-full-tag-commit-sha
gh attestation verify stuntmaster-pc-0.0.1-windows-x64.zip \
  --repo neonoxd/psyx-stuntmaster \
  --signer-workflow github.com/neonoxd/psyx-stuntmaster/.github/workflows/windows-build.yml \
  --signer-digest "$TAG_COMMIT_SHA" \
  --source-digest "$TAG_COMMIT_SHA" \
  --predicate-type https://spdx.dev/Document/v2.3 \
  --deny-self-hosted-runners
```

The other five release files, including the Markdown report, can be verified
the same way with `--predicate-type https://slsa.dev/provenance/v1`. The draft
release workflow performs both forms of verification automatically for all
published files and rejects self-hosted-runner attestations. Pinning the tag's
full commit as both the signer-workflow and source digest is essential: trusted
branch and manual builds also create valid repository attestations, but they
are not release approval.

## Versioning

The CMake `project(... VERSION ...)` value is the release-package version and
drives CPack's archive name. `vcpkg.json` mirrors that version because vcpkg
requires its own manifest version. CI rejects a mismatch between the two.

The workflow discovers the generated archive and extracts its version instead
of hardcoding `0.0.1`. A normal version bump therefore changes the CMake
project version and the matching `version-string` in `vcpkg.json`; workflow
paths do not need editing.

Version identifiers and tag naming remain maintainer policy. The workflow does
not require SemVer, impose a tag pattern, derive a tag from the CMake version,
or require the tag and package version to match. Pushing any intentionally
chosen tag asks CI to promote a successful build of that exact commit into a
draft release named verbatim after the tag. The maintainer remains responsible
for choosing the project version, choosing the tag, writing release notes,
selecting prerelease or latest status, and publishing the draft.

## Build reuse and performance

The Windows job uses a repository-local Release-only vcpkg triplet. vcpkg no
longer compiles unused Debug variants of fmt, OpenAL Soft, or SDL2 for a Release
package. Their binary packages are stored in a GitHub Actions cache keyed by the
pinned vcpkg manifest and triplet. A compatible cache is restored in later
workflow runs, while vcpkg's own ABI keys decide whether each package can be
reused. The project itself is always compiled and tested from the checked-out
source.

Cached dependency binaries are build inputs, but they are not release artifacts
or proof of provenance by themselves. vcpkg accepts an entry only when its ABI
key matches the current port, triplet, and toolchain configuration; the complete
application is still linked and tested on every run. The cache does not contain
credentials, source trees, FFmpeg, project objects, or final artifacts. GitHub
scopes caches by branch/ref: trusted default-branch caches can be restored by
later branches and pull requests, while pull-request cache writes cannot
populate the default branch's cache. A cold default-branch run populates the
reusable cache after dependencies or the triplet change.

## Reproducibility

Release compilation uses MSVC's `/Brepro` option to remove compiler and linker
timestamps, including PsyCross's legacy `__DATE__` and `__TIME__` banner. The
build derives `SOURCE_DATE_EPOCH` from the checked-out commit, which gives CPack
stable ZIP entry timestamps. The SBOM likewise uses the source commit time and
a namespace derived from the source commit and archive digest. For the same
source, dependency set, and toolchain, independent clean runs are therefore
expected to produce the same release ZIP and SBOM SHA-256 values.

`windows-2022` selects a stable runner family but not an immutable Visual Studio
or Windows SDK image. CI records the runner image, Visual Studio, CMake, and Git
versions so a future toolchain update is visible. Bit-for-bit identity across a
toolchain update is not promised; establishing that stronger guarantee would
require distributing a fully pinned Windows toolchain or build image.

The existing `0.0.1-BETA` Release predates this workflow and remains the
maintainer-uploaded asset; these checks do not retroactively attest or replace
it. They apply to artifacts and future draft Releases created by these
workflows.

## SBOM scope

The authoritative SPDX 2.3 JSON SBOM describes the packaged Windows
distribution. It includes
every file in the release ZIP with SHA-1 and SHA-256, the exact source and
PsyCross commits, the pinned FFmpeg NuGet package and checksum, and every
package recorded in the build's vcpkg status database. Static dependencies are
represented as package relationships; shipped FFmpeg DLLs and license files
are associated with the FFmpeg package. The analyzed `stuntmaster-pc`
distribution contains every shipped file. Files copied from an unanalyzed
dependency package use SPDX `GENERATED_FROM` relationships, which preserves
their component provenance without claiming that every file in the complete
upstream package was analyzed.

The build also derives a deterministic Markdown companion from the same SBOM
model. It presents release provenance, packages, roles, declared and concluded
licenses, and every shipped file's SHA-256 in tables intended for people.
Markdown is not an SPDX serialization, so the report labels the `.spdx.json`
file as authoritative.
Both files are published beside the release ZIP, not embedded within it: this
avoids self-referential archive hashing and keeps the SBOM available before a
consumer downloads or runs the package. The files are also retained together in
the Actions artifact for non-release builds.

The Windows job captures stable build-environment facts after configuration:
the GitHub runner image and image revision, architecture, Visual Studio and MSVC
versions, MSVC toolset and tools version, Windows SDK, CMake version/generator,
Git version, Release configuration, vcpkg triplet and baseline,
`SOURCE_DATE_EPOCH`, and the active reproducibility options. It also records the
exact SHA-256 and Authenticode result for the MSVC compiler, linker, librarian,
MSBuild, Windows resource and manifest tools, CMake, CTest, CPack, vcpkg, Git,
and PowerShell. The Microsoft build and SDK tools must have a valid Microsoft
publisher signature. Any invalid signature fails the build; tools that are
legitimately unsigned remain identified by their byte hash and an explicit
`NotSigned` result.

These records are collected after compilation and gate artifact publication.
They identify the selected executable bytes but are not a process trace and do
not hash every DLL, header, SDK library, MSBuild target, or operating-system
component loaded by those tools. The runner image revision and repeated-build
comparisons remain complementary evidence rather than a hermetic toolchain
closure.

An aggregate toolchain digest covers the ordered tool records, including each
file hash, signature status, signer identity, and signing-certificate
thumbprint. The authoritative SPDX JSON stores all structured values in an SPDX
`OTHER` document annotation, while the Markdown companion renders dedicated
build-environment and tool-integrity tables. Because that SPDX document is the
signed SBOM predicate, changing any environment or tool-integrity value breaks
attestation verification. The Markdown is independently covered as a subject
of the second provenance attestation.

Run IDs, attempt numbers, temporary workspace paths, and other incidental
per-run values are deliberately excluded. They do not describe the compiler or
artifact and would make otherwise identical SBOM outputs differ. Build tools
remain non-shipped provenance rather than packages or runtime dependencies in
the artifact-scoped package graph. CI passes the actual `owner/repository` into
the SBOM generator so fork artifacts identify the fork that supplied their
source commit; the pinned PsyCross source continues to identify its own
`neonoxd/PsyCross` repository.
