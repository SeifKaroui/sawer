# Releasing Sawer for Windows

Sawer publishes two Windows AMD64 choices from the same tested executable:

    Sawer-Setup.exe
    Sawer-Portable.exe

`Sawer-Setup.exe` is the primary download. It performs a per-user installation
without elevation, creates Start Menu and desktop shortcuts, registers Sawer
as a `.sawer` handler, and launches Sawer after one Install confirmation.
`Sawer-Portable.exe` runs directly without installing anything.

Both application forms contain the icon, Windows version metadata, embedded
fonts, and complete third-party notices. GitHub Releases also carry
`SHA256SUMS.txt` and a browser-readable copy of `THIRD_PARTY_NOTICES.md`.

## Prepare a release

1. Update the version in `meson.build`, `src/core/BuildInfo.hpp`, and
   `tests/core/BuildInfoTests.cpp`.
2. Configure `build/` as a Release build and run the complete local test set.
3. If `assets/logo.png` changed, regenerate the committed Windows icon:

       python -m pip install Pillow==12.3.0
       python tools/generate_windows_icon.py assets/logo.png assets/windows/Sawer.ico

4. Run the `Windows release` workflow manually. It installs the signed NSIS
   3.12-1 package through the existing MSYS2 environment, rejects any compiler
   version drift, and creates a tested Actions artifact without publishing a
   GitHub Release.
5. Download the artifact and validate both choices on supported Windows 10 and
   Windows 11 systems.

For the installer, confirm:

- Running it requires only the Install confirmation and does not request
  administrator credentials.
- Sawer launches after installation and its shortcuts work.
- Double-clicking a `.sawer` board opens it in Sawer.
- Running a newer installer upgrades in place.
- Uninstall removes the application and shortcuts without deleting boards or
  preferences.

For the portable version, confirm startup, drawing, save/open, icon display,
`--version`, and `--third-party-notices`.

## Publish

Create and push an annotated tag that exactly matches Sawer's version:

    git tag -a vX.Y.Z -m "Sawer X.Y.Z"
    git push origin vX.Y.Z

The tag workflow repeats the clean build and release checks, attests both
executables, generates release notes, and publishes the installer, portable
version, checksum manifest, and notices. Versions below 1.0.0 are marked as
prereleases.

The workflow rejects malformed tags and any disagreement between the tag,
Meson project version, `BuildInfo`, version test, runtime `--version` output,
embedded notices, installer version, or Windows executable metadata. It also
performs silent install, reinstall, and uninstall checks in a Unicode path and
verifies shortcuts, file registration, and board preservation.

## If publication fails

Do not move or replace a published release tag. Correct the source or workflow,
increment the version, and publish a new tag. A failed workflow that did not
create a release can be rerun after correcting an external transient failure.

## Current limitation

Windows releases are not Authenticode-signed. The checksums and GitHub build
provenance establish integrity and origin, but Windows SmartScreen may still
warn users. The installer does not use MSIX, the Microsoft Store, or a
Microsoft developer account.
