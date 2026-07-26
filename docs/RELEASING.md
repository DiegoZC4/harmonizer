# Releasing

Releases are built by GitHub Actions from an exact Git tag. Do not upload
locally built binaries to a release.

## Normal release

1. Update `VERSION.txt`.
2. Move completed entries from `Unreleased` into a dated version section in
   `CHANGELOG.md`.
3. Run `./scripts/check_release.sh`.
4. Commit and push the change.
5. Tag the exact commit with `git tag -s v$(cat VERSION.txt)` when a signing key is
   available, otherwise use an annotated tag:

   ```bash
   git tag -a "v$(cat VERSION.txt)" -m "Harmonizer $(cat VERSION.txt)"
   git push origin "v$(cat VERSION.txt)"
   ```

The release workflow builds:

- `Harmonizer-macOS-universal.dmg`
- `Harmonizer-macOS-universal.zip`
- `Harmonizer-Windows-x64-Setup.exe`
- `Harmonizer-Windows-x64.zip`
- `Harmonizer-Linux-x86_64.AppImage`
- `Harmonizer-source.zip`
- `SHA256SUMS.txt`

## Signing secrets

Unsigned packages are useful for development but show operating-system
warnings. Configure these repository secrets for polished public releases:

### Apple

- `APPLE_CERTIFICATE_P12_BASE64`
- `APPLE_CERTIFICATE_PASSWORD`
- `APPLE_SIGNING_IDENTITY`
- `APPLE_ID`
- `APPLE_TEAM_ID`
- `APPLE_APP_PASSWORD`

The workflow imports the Developer ID certificate, enables hardened runtime,
signs the universal app, submits the DMG to Apple's notary service, and staples
the ticket.

### Windows

- `WINDOWS_CERTIFICATE_PFX_BASE64`
- `WINDOWS_CERTIFICATE_PASSWORD`

The workflow signs the application executable and installer with SHA-256 and a
trusted timestamp.

Never place certificates or passwords in the repository.

## Failed release

Fix the cause on `main`, increment the patch version, and create a new tag.
Release tags and published artifacts are immutable records; do not silently
replace them.
