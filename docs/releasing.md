# Release builds

The workflow builds `sawer-vX.Y.Z-windows-x64-setup.exe` (per-user installer) and
`sawer-vX.Y.Z-windows-x64-portable.exe` from one statically linked executable. Fonts, icons,
version metadata, and third-party notices are embedded. Tag runs publish
checksums and build provenance. Binaries are not Authenticode-signed.

Download filenames and Actions artifact names use lowercase `sawer-vX.Y.Z`,
with `windows-x64` or `linux-x64`. The conventional `.AppImage` extension is
preserved. Replace `X.Y.Z` in the examples below with the source version.
The installed executable and displayed application name remain `Sawer`.

## Shared CI compiler

Windows and Linux use GCC **15.2.0**, selected by `tools/gcc-toolchain.json`.
Linux installs the exact Ubuntu 22.04 compiler package from the Ubuntu
Toolchain PPA. Windows downloads the complete WinLibs UCRT/POSIX toolchain,
verifies its SHA-256, and selects its compiler and binutils ahead of MSYS2
build tools. MSYS2 continues to provide the shell and build dependencies.
Both workflows check the C and C++ compiler versions and x86_64 target,
then compile `BoardPath.cpp` to verify its C++20 `clock_cast` support before
building dependencies. Cached Windows toolchains undergo these checks too.

The Linux executable requires a compatible system C++ runtime; the Ubuntu
24.04 compatibility job installs it from the same PPA. The AppImage bundles
its compiler runtime and first passes a version/notices check on stock Ubuntu
24.04 before that installation. Updating CI compilers requires changing the
shared pin, Linux package revision, and Windows archive URL/checksum together.

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
and checksum assets, accepting both versioned filenames and legacy
`Sawer-Setup.exe` / `SHA256SUMS.txt` downloads. The first release reports that
scenario as skipped. A previous application does not have to support the current board format.

## Workflow and publication

The Windows and Linux workflows can run manually and are reusable jobs in
`.github/workflows/release.yml`. Only the Release workflow handles version-tag
pushes and publishes downloads. Hosted tests cover
headless unit, asset-fetch, Unicode file commands, portable documentation,
installer preservation helpers, and version/notices checks. GPU/window/performance checks must also pass
locally on the exact Release source. Validate Windows 10 and 11 interactively.

A manual platform run creates an unpublished artifact. A matching tag starts
both platforms from the same commit. Publication waits for Windows installer
checks, both Linux build types, and all Ubuntu 24.04 compatibility jobs.

`tools/publish_release.py` verifies the exact asset sets and checksums, confirms
that the tag points to the tested commit, and uploads both platforms to one
draft. It downloads and compares the uploaded bytes before making the release
public. Failed uploads or verification leave the draft unpublished; retries
can resume that draft. Published releases are never overwritten.

The workflow uses GitHub's provided token; no personal token is required.
After local Release validation, commit the intended source and push it, then:

```text
git tag -a vX.Y.Z -m "Sawer X.Y.Z"
git push origin vX.Y.Z
```

Tags must match the runtime/source version. Versions below 1.0.0 are
prereleases. Do not move a published tag; release a new version for fixes.
Linux archives require compatible system libraries. The Linux pipeline is
described below and produces executable, AppImage, and Flatpak downloads.

## Linux executable, AppImage, and Flatpak pipeline

`.github/workflows/linux-release.yml` runs on pull requests, manually, and on
version tags. It builds all Meson targets in Debug and Release on Ubuntu
22.04 x86_64, using GCC 15.2.0, Meson 1.11.2, and CMake 3.31.6. Each job uses only
`build/`; Release wipes that directory after the initial Debug configuration.
Dependencies and fonts retain the existing version/hash pins and download
caches. Both SDL display backends and Vulkan are enabled explicitly.

The `headless` suite covers document/storage/geometry tests, Unicode paths,
font-fetch recovery, package contents, versions, and embedded notices. The
`graphics` suite runs serially under Xvfb/Openbox and headless Weston, forcing
the requested SDL backend and Mesa lavapipe Vulkan. Isolated XDG directories
keep test preferences away from the runner account. X11 waits for Openbox's
post-initialization startup hook before launching clients: its WM property is
published too early to guarantee window-mapping events are handled. Backend
processes are terminated on success and failure; readiness and command waits are bounded.
Each download/Flatpak command has a 120-second deadline; full graphics suites
allow 900 seconds. Timeout cleanup stops the command's process group, including
launcher children. Logs include command output, display diagnostics, application
logs from the isolated preferences, and process/thread wait locations on timeout.
Build and display logs are retained even when checks fail.

Software rendering exercises correctness, including pixel checks, resize,
input isolation, board transitions, and renderer recovery. It does not
establish hardware performance: the `performance` suite, multi-monitor/DPI
checks, and real Intel/AMD/NVIDIA validation remain required release checks.

Release runs generate the following artifact contents:

- `sawer-vX.Y.Z-linux-x64`: the stripped, statically linked SDL executable.
- `sawer-vX.Y.Z-linux-x64.AppImage`: that exact executable plus compiler runtime
  libraries, launcher, desktop metadata, icon, documentation, and notices.
- `sawer-vX.Y.Z-linux-x64.flatpak`: an installable bundle of the same executable and
  compiler runtimes, with desktop, file-type, and application metadata.
- `sawer-vX.Y.Z-linux-x64-flatpak-runtime.txt`: the Freedesktop runtime branch and
  runtime/graphics-extension commits used for the Flatpak checks.
- `sawer-vX.Y.Z-linux-x64-sha256sums.txt`: checksums for the downloads and companion
  dependency/notice reports.
- `sawer-vX.Y.Z-linux-x64-dependencies.txt`: the plain executable's shared dependencies.
- `sawer-vX.Y.Z-linux-x64-third-party-notices.md`: application, runtime, and compiler notices.

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
X11 and Wayland. Four independent Ubuntu 24.04 jobs download the actual CI
artifact, check its checksums, restore executable permissions, and repeat startup
and presentation checks for each file/backend pair. A failure does not cancel the
other checks. AppImage graphics run on the stock distribution runtime; only the
standalone executable jobs install the newer system C++ runtime.

Run **Linux release** manually from Actions to generate the
`sawer-v<version>-linux-x64` artifact. Artifact ZIPs and browser downloads may
lose executable permissions; use `chmod +x sawer-vX.Y.Z-linux-x64 sawer-vX.Y.Z-linux-x64.AppImage`.
Manual Linux runs generate and validate artifacts only. Tag releases publish
the tested Linux assets alongside Windows through the shared Release workflow.

## Flatpak packaging and installation

Release jobs reuse the verified AppDir payload through `tools/package_flatpak.py`.
The generated manifest and payload are staged under `build/flatpak-source/`;
`flatpak-builder` installs those files into `/app` without compiling Sawer again.
This preserves the shared GCC 15.2.0 compiler choice. The builder disables
stripping and debug-info extraction so the executable remains byte-identical.
Compiler runtime libraries and their licenses are retained. The existing
application icon supplies the 256px PNG without a separate image dependency.

Packaging uses the official Flatpak stable PPA's pinned Ubuntu 22.04 packages:
Flatpak 1.18.4, flatpak-builder 1.4.8, and AppStream 0.15.2. The modern builder
composes application metadata on the host, avoiding obsolete SDK helper tools.
`packaging/linux/io.sawer.app.json` selects the Freedesktop Platform and SDK
25.08 branch. Supported runtime updates within that branch are obtained from
Flathub; their installed commits are recorded with the artifact. The SDK is
needed only for packaging. The installable bundle points to the official
Flathub runtime repository and includes the application, rather than an offline
copy of the complete runtime. See the official
[single-file bundle documentation](https://docs.flatpak.org/en/latest/single-file-bundles.html).

The workflow installs the actual `.flatpak` file into a disposable per-user
installation at `.cache/flatpak-user`. It compares the installed executable and
notices with the release payload, checks the sandboxed version/notices commands,
and presents frames on X11 and Wayland. Graphics checks select lavapipe from
inside the runtime; host Vulkan ICD paths are not used by the sandbox. Display
logs are retained under `build/flatpak-logs/`. Flatpak staging, state, and export
repositories stay under `build/`; they are packaging outputs, not Meson build
configurations. The bundle is published with the other release assets; there
is no Flathub submission.

After installing Flatpak, users can install and run the artifact with:

    flatpak install --user ./sawer-vX.Y.Z-linux-x64.flatpak
    flatpak run io.sawer.app

The sandbox allows Wayland, fallback X11, graphics devices, and Documents.
It grants neither network access nor access to the entire home or host
filesystem. Documents provides directory access required by atomic compaction,
rename, and conflict-copy creation. For boards in other locations, users must
allow the containing folder, for example:

    flatpak override --user --filesystem=/absolute/path/to/boards io.sawer.app

A portal grant for one selected file alone does not permit Sawer's adjacent
transaction/recovery files. These grants do not change Sawer's behavior: it
opens requested boards and never scans or manages the containing folder.
Preferences and previews use the sandbox's application data directories.
Permission behavior is described in the official
[sandbox documentation](https://docs.flatpak.org/en/latest/sandbox-permissions.html).
Desktop file launching, native file chooser portals, removable drives, saving,
rename, conflict handling, and install/uninstall should also be verified on real
desktops before a public release.

To rebuild locally after preparing the licensed AppDir payload and installing
the runtime/SDK as shown in the workflow:

    python tools/package_flatpak.py --appdir build/Sawer.AppDir --output build/flatpak-source --version X.Y.Z --release-date YYYY-MM-DD
    flatpak-builder --user --arch=x86_64 --disable-rofiles-fuse --disable-cache --state-dir=build/flatpak-state --repo=build/flatpak-repo build/Sawer.Flatpak build/flatpak-source/io.sawer.app.json
    flatpak build-bundle --arch=x86_64 --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo build/flatpak-repo build/linux-dist/sawer-vX.Y.Z-linux-x64.flatpak io.sawer.app stable

Use a fresh staging/output directory and the source version and commit date.
After installing the bundle, run the same metadata and presentation checks:

    dbus-run-session -- python tools/test_flatpak.py --executable build/linux-dist/sawer-vX.Y.Z-linux-x64 --graphics

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
