# Release builds

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
CI reuses MSYS2 package downloads, pip wheels, Meson dependency archives,
verified fonts, and the extracted NSIS installation. Download caches are saved
after the build, before tests, so later failures do not discard them. Cache
keys include dependency definitions; NSIS and fonts retain their hash checks.
Meson configures a fresh `build/` each run. The installer invokes NSIS by its
exported absolute path because MSYS2 filters the inherited Windows PATH.
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
Preservation hashes exclude the active diagnostic `Sawer.log` and disposable
`previews/` cache; settings and other user files remain checked.

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
Linux archives require compatible system libraries. The Linux pipeline is
described below; Flatpak remains separate work.

## Linux executable and AppImage pipeline

`.github/workflows/linux-release.yml` runs on pull requests, manually, and on
version tags. It builds all Meson targets in Debug and Release on Ubuntu
22.04 x86_64, using GCC 12, Meson 1.11.2, and CMake 3.31.6. Each job uses only
`build/`; Release wipes that directory after the initial Debug configuration.
Dependencies and fonts retain the existing version/hash pins and download
caches. Both SDL display backends and Vulkan are enabled explicitly.

The `headless` suite covers document/storage/geometry tests, Unicode paths,
font-fetch recovery, package contents, versions, and embedded notices. The
`graphics` suite runs serially under Xvfb/Openbox and headless Weston, forcing
the requested SDL backend and Mesa lavapipe Vulkan. Isolated XDG directories
keep test preferences away from the runner account. Backend processes are
terminated on success and failure; readiness and command waits are bounded.
Build and display logs are retained even when checks fail.

Software rendering exercises correctness, including pixel checks, resize,
input isolation, board transitions, and renderer recovery. It does not
establish hardware performance: the `performance` suite, multi-monitor/DPI
checks, and real Intel/AMD/NVIDIA validation remain required release checks.

Release runs generate the following artifact contents:

- `Sawer`: the stripped, statically linked SDL executable.
- `Sawer-x86_64.AppImage`: that exact executable plus compiler runtime
  libraries, launcher, desktop metadata, icon, documentation, and notices.
- `SHA256SUMS-Linux-x86_64.txt`: checksums for the downloads and companion
  dependency/notice reports.
- `DEPENDENCIES-Linux-x86_64.txt`: the plain executable's shared dependencies.
- `THIRD_PARTY_NOTICES-Linux.md`: application, runtime, and compiler notices.

The AppDir preparation rejects unresolved libraries, shared SDL, unexpected
dependencies, wrong architectures, and existing output directories. It copies
compiler runtime libraries by SONAME and leaves glibc, display libraries,
Vulkan, and hardware drivers to the operating system. This is an Ubuntu 22.04
(glibc 2.35) compatibility baseline, not a promise to run on every distribution.
The plain executable also needs compatible libstdc++/libgcc system packages.

AppImage tooling is pinned to appimagetool 1.9.1 (SHA-256
`ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0`)
and type2-runtime 20251108 (SHA-256
`2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d`).
Both are downloaded from versioned upstream releases and verified on cache
hits as well as misses. Supplying `--runtime-file` prevents appimagetool from
fetching an unpinned runtime. Tool execution and download checks use extraction
rather than requiring a FUSE mount in CI.

Payload extraction verifies that the application matches the plain download
byte for byte and that executable modes, metadata, and notices survived
packaging. Both downloads run version/notices checks and present frames under
X11 and Wayland. A separate Ubuntu 24.04 job downloads the actual CI artifact,
checks its checksums, restores executable permissions, and repeats startup and
presentation checks for both files.

Run **Linux release** manually from Actions to generate the
`Sawer-<version>-Linux-x86_64` artifact. Artifact ZIPs and browser downloads may
lose executable permissions; use `chmod +x Sawer Sawer-x86_64.AppImage`.
These runs generate and validate artifacts only. Linux release publication
requires a separate authorized step; the existing Windows workflow is unchanged.

## Rebuild an AppImage locally

After a Linux Release build, download and checksum-verify the two pinned
AppImage tools as shown in the workflow. Then stage the application:

    python tools/package_linux.py --executable build/Sawer --appdir build/Sawer.AppDir
    desktop-file-validate build/Sawer.AppDir/io.sawer.app.desktop

The output directory must be new. The workflow also copies the GCC copyright
and GNU license texts into the payload; preserve those steps when redistributing
bundled compiler libraries. Extract appimagetool in `build/appimagetool/`, then
run its extracted `squashfs-root/AppRun` with `ARCH=x86_64`,
`--no-appstream`, the verified `--runtime-file`, the staged AppDir, and the
desired output path. The AppImage runtime notices are preserved in
`packaging/linux/AppImage-runtime-LICENSE.txt`; upstream source and rebuild
instructions are available at
[the pinned runtime source](https://github.com/AppImage/type2-runtime/tree/20251108).

The AppImage launcher preserves the caller's working directory and quoted
Unicode arguments, so relative board paths continue to work. Desktop metadata
is packaged but this portable workflow does not install system file associations.
