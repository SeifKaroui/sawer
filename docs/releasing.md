# Windows releases

The workflow builds `Sawer-Setup.exe` (per-user installer) and
`Sawer-Portable.exe` from one statically linked executable. Fonts, icons,
version metadata, and third-party notices are embedded. Tag runs publish
checksums and build provenance. Binaries are not Authenticode-signed.

## Build and package

Keep the version consistent in `meson.build`, `src/core/BuildInfo.hpp`, and
`tests/core/BuildInfoTests.cpp`. Use the single `build/` directory:

```text
meson setup build --wipe --buildtype=release -Dsawer_werror=true
meson compile -C build
meson test -C build --no-rebuild --num-processes 1 --print-errorlogs
python tools/check_release.py --source-root . --executable build/Sawer.exe
meson compile -C build package
```

Omit `--wipe` for a fresh checkout. Regenerate a changed Windows icon with
Pillow 12.3.0 and `tools/generate_windows_icon.py`. Archives and Meson
installation preserve `assets/banner.png`, technical documents under `docs/`,
and linked shader references.

CI downloads the official NSIS 3.12 ZIP through HTTPS-only mirror fallback,
verified with SHA-256
`56581f90db321581c5381193d796fffcf2d24b2f8fed2160a6c6a3baa67f2c4f`.
A running or unwritable application blocks installation/uninstall with exit code 2;
application extraction failure returns 3. Uninstall preserves boards and
preferences.

## Installer checks

`tools/test_windows_installer.ps1` requires a disposable account with no
existing Sawer registration, shortcuts, preferences, or `.sawer` association.
It checks executable contents/metadata, shortcut targets and launches, a
valid Unicode-named board opened through its association, blocked upgrade
and uninstall while Sawer is running, reinstall, byte preservation of boards
and preferences, and preservation of another default handler. It closes only
processes started by the test. `-SkipApplicationLaunch` explicitly skips GUI
launches on runners without SDL GPU; a held executable handle still tests
locked-file protection. GUI launches must pass on supported hardware.

Pass `-PreviousInstaller` and `-PreviousVersion` for an older-version upgrade.
The workflow retrieves the most recent lower published version with installer
and checksum assets. The first release reports that scenario as skipped. A
previous application does not have to support the current board format.

## Workflow and publication

The Windows workflow runs manually and on version tags. Hosted tests cover
headless unit, asset-fetch, Unicode file commands, portable documentation,
installer preservation helpers, and version/notices checks. GPU/window/performance checks must also pass
locally on the exact Release source. Validate Windows 10 and 11 interactively.

A manual branch run creates an unpublished artifact. Matching tags publish
automatically. After artifact validation:

```text
git tag -a vX.Y.Z -m "Sawer X.Y.Z"
git push origin vX.Y.Z
```

Tags must match the runtime/source version. Versions below 1.0.0 are
prereleases. Do not move a published tag; release a new version for fixes.
Linux archives require compatible system libraries; AppImage/Flatpak
packaging and Wayland/X11 validation remain separate work.
