# Releasing Sawer for Windows

Sawer's primary Windows release artifact is a statically linked, directly
downloadable executable named:

    Sawer-X.Y.Z-Windows-AMD64.exe

The executable contains the application icon, Windows version metadata,
embedded fonts, and complete third-party notices. GitHub Releases also carry a
SHA-256 checksum and a browser-readable copy of `THIRD_PARTY_NOTICES.md`.

## Prepare a release

1. Update the version in `meson.build`, `src/core/BuildInfo.hpp`, and
   `tests/core/BuildInfoTests.cpp`.
2. Configure `build/` as a Release build and run the complete local test set.
3. If `assets/logo.png` changed, regenerate the committed Windows icon:

       python -m pip install Pillow==12.3.0
       python tools/generate_windows_icon.py assets/logo.png assets/windows/Sawer.ico

4. Run the `Windows release` workflow manually. This creates a tested Actions
   artifact without publishing a GitHub Release.
5. Download the artifact and validate the executable on supported Windows 10
   and Windows 11 systems, including icon display, startup, drawing, save/open,
   and `--third-party-notices`.

## Publish

Create and push an annotated tag that exactly matches Sawer's version:

    git tag -a vX.Y.Z -m "Sawer X.Y.Z"
    git push origin vX.Y.Z

The tag workflow repeats the clean build and release checks, attests the
executable, generates release notes, and publishes the executable, checksum,
and notices. Versions below 1.0.0 are marked as prereleases.

The workflow rejects malformed tags and any disagreement between the tag,
Meson project version, `BuildInfo`, version test, runtime `--version` output,
embedded notices, or Windows executable metadata.

## If publication fails

Do not move or replace a published release tag. Correct the source or workflow,
increment the version, and publish a new tag. A failed workflow that did not
create a release can be rerun after correcting an external transient failure.

## Current limitation

Windows releases are not yet Authenticode-signed. The checksum and GitHub build
provenance establish integrity and origin, but Windows SmartScreen may still
warn users until code signing is added.
