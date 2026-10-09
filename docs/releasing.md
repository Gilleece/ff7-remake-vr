# Releasing

## Making a release

1. Commit, then tag: `git tag v1.3` and push the tag (`git push origin v1.3`).
2. The workflow `.github/workflows/release.yml` runs on the tag (and by hand through
   "Run workflow"). On `windows-latest` it builds both packages from a clean checkout
   with `tools\package\package.ps1` and `package.ps1 -Dlss`, checks the four zips
   against the SHA-256 sums `package.ps1` wrote, attests their build provenance
   (`actions/attest-build-provenance`), uploads the zips and `SHA256SUMS.txt` as the
   run's artifact `ff7vr-packages` and, for a tag, creates a **draft** release with the
   zips, `SHA256SUMS.txt` and a "Verify your download" section as its notes.
3. Read the draft, add the changes to the notes, publish it.

The version in the DLL's log line, its version resource and the first line of
`VERSION.txt` comes from `git describe --tags --always --dirty` at build time
(`cmake/ff7vr_buildinfo_write.cmake`): `v1.3` on the tag, `v1.3-2-gabc1234` after it,
`-dirty` with uncommitted changes, `unknown` without git.

## Where the hashes come from

`package.ps1` writes, next to the zips in `dist\`:

- `<package>-SHA256SUMS.txt`: one line per zip, `<sha256>  <file>` (the `sha256sum`
  format), for the package zip and its `-dropin.zip`;
- `<package>-release-notes-snippet.md`: the same hashes and how to check them.

A local build gives different hashes from the workflow's (the build time and paths
end up in the files). The hashes in a release are the ones of the files attached to it.

## Verifying a download

- Hash, any player: `certutil -hashfile <zip> SHA256` (or PowerShell
  `Get-FileHash <zip>`), compared with the release notes or `SHA256SUMS.txt`.
- Provenance (built by this repository's workflow from the tagged commit), with the
  GitHub CLI: `gh attestation verify <zip> --repo <owner>/ff7-remake-vr`, where
  `<owner>` is the account the release is published under.

## Repository settings (by hand)

- Turn on **immutable releases** (Settings, General, Releases): once published, a
  release's tag and assets can no longer be changed.
- Never replace an asset of a published release. A fix is a new version with a new tag.
