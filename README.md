<p align="center">
  <img src="assets/banner.png" alt="Sawer banner" width="100%">
</p>

# Sawer

Sawer is a simple, fast, native whiteboard for Windows and Linux. It is built
around local files: each board is one portable `.sawer` file that you can keep
in any ordinary folder, copy, move, and share.

> **Status:** Pre-release — automated Windows AMD64 installer and portable
> builds are configured; Linux binaries still need to be built from source.

## Download

### Installer (recommended)

Download `Sawer-Setup.exe` from the
[GitHub Releases page](https://github.com/SeifKaroui/sawer/releases), run it,
and choose **Install**. Sawer installs for your Windows account without an
administrator prompt, adds Start Menu and desktop shortcuts, registers
`.sawer` boards, and opens automatically. No account or restart is required.

### Portable version

Download `Sawer-Portable.exe` from the same release if you prefer not to
install anything. Save it anywhere and run it directly; the application is
self-contained and has no companion-file requirement.

Each release includes `SHA256SUMS.txt`. Verify either download from PowerShell
with:

    Get-FileHash .\Sawer-Setup.exe -Algorithm SHA256
    Get-FileHash .\Sawer-Portable.exe -Algorithm SHA256

Compare the result with the matching line in `SHA256SUMS.txt`. Windows binaries
are not yet code-signed, so SmartScreen may show a warning.

## Built around the essentials

- **Simple.** A focused, mouse-first drawing experience with only the tools
  needed for freehand notes and basic shapes—without a sprawling feature set.
- **Fast.** Native rendering keeps drawing, editing, panning, and zooming
  responsive, including on large boards and high-DPI displays.
- **Portable.** A board is a self-contained file, not a project or managed
  workspace. Open it from anywhere and take it with you.
- **Private.** There are no accounts, cloud services, telemetry, or database.
  Your boards remain ordinary files under your control.

## What works today

- Draw freehand strokes, lines, rectangles, and ellipses with configurable
  colors, widths, and shape fills.
- Select, marquee-select, move, resize, duplicate, delete, and restyle board
  objects with undo and redo.
- Create, open, save, autosave, and safely recover independent `.sawer`
  boards. Recent boards appear on Home.
- Pan a finite board and zoom beneath the cursor. Rendering is designed for
  large boards and high-DPI displays.
- Run natively with SDL GPU using Direct3D 12 on Windows or Vulkan on Linux.

## In progress or deferred

- SDL Render fallback, complete device-loss recovery, and broad mixed-DPI
  validation are still in progress.
- Linux Flatpak/AppImage packaging remains a future milestone.
- Pen, touch, text, images, PDF import, sync, collaboration, and plugins are
  intentionally deferred until after the mouse-first 1.0 release.

## Quick start

### Build and run

Install the [requirements](#requirements), then configure, build, test, and
run a debug build on Windows:

    meson setup build --buildtype=debug -Dsawer_werror=true
    meson compile -C build
    meson test -C build --print-errorlogs
    build/Sawer.exe

On Linux, use the same `build` directory and run `build/Sawer`. See
[Build on Linux](#build-on-linux) for the complete commands.

### Create and save a board

1. On Home, choose **New board**, draw with the Pencil, then choose **Save
   As** to select any destination.
2. To reopen it later, use **Open board**, click it from Home’s recent-board
   list, or pass the file path when launching Sawer.
3. Sawer autosaves completed actions after roughly one second. Press `Ctrl+S`
   to flush immediately.

Create a new board at a chosen path, or open an existing one:

    build/Sawer.exe drawings/ideas.sawer

## Controls

| Task | Control |
| --- | --- |
| Pan | Middle-drag, or `Space` + left-drag |
| Zoom | Mouse wheel beneath the cursor |
| Choose Pencil / Line / Rectangle / Ellipse | `P` / `L` / `R` / `E` |
| Draw | Left-drag with the current tool |
| Constrain a line or shape | Hold `Shift` while drawing |
| Toggle shape fill | `F` |
| Select and edit objects | `V`, then click or drag a marquee |
| Move or resize a selection | Drag selected objects or their handles |
| Duplicate / delete selection | `Ctrl+D` / `Delete` |
| Undo / redo | `Ctrl+Z` / `Ctrl+Shift+Z` or `Ctrl+Y` |
| Cancel the active gesture | `Escape` |
| Choose color / adjust width | `1`–`7` / `[` and `]` |
| New / Open / Save / Save As | `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` |
| Toggle theme | `T` |

The toolbar also provides file, drawing, style, history, zoom, Home, and
theme controls. Hover a control to see its shortcut and purpose. The board is
finite and clamped to +/-1,000,000 logical units.

## Your boards stay yours

Save a board wherever you like, then copy, move, back up, or share that one
file whenever you need to. Sawer does not require an account or take ownership
of a folder. It autosaves your completed work and makes saving problems clear
so you can choose what to do next.

## Large boards

Sawer is built to keep normal frame work tied to what is visible, rather than
the total size of a board. Viewport queries, retained geometry, zoom-adaptive
stroke detail, and bounded caches support 100,000 mixed objects, one million
freehand points, thousands of visible objects, and extreme zoom transitions in
the automated regression workload.

## Requirements

- Meson 1.7 or newer and Ninja.
- A C++20 compiler: MSVC, GCC, or Clang.
- CMake 3.28 or newer. Meson uses it internally to build SDL3 because SDL3
  does not provide an upstream Meson definition.
- Network access during the first configuration to download pinned
  dependencies.

Git is not required to configure or build Sawer. SDL3 is linked statically on
Windows and Linux. Windows MinGW release and development builds also link the
GCC C++ runtime statically, so Sawer can be distributed as one executable
without third-party DLLs. Linux builds embed SDL3 but use the operating
system’s standard display, graphics, audio, and C runtime libraries.

## Build on Windows

For a debug build:

    meson setup build --buildtype=debug -Dsawer_werror=true
    meson compile -C build
    meson test -C build --print-errorlogs

For an optimized build, reconfigure the same directory:

    meson setup build --wipe --buildtype=release -Dsawer_werror=true
    meson compile -C build
    meson test -C build --print-errorlogs

## Build on Linux

For a debug build:

    meson setup build --buildtype=debug -Dsawer_werror=true
    meson compile -C build
    meson test -C build --print-errorlogs

For an optimized build, use `meson setup build --wipe --buildtype=release`.

## Run and diagnose

Run the Windows debug executable:

    build/Sawer.exe

Close the window normally to exit. Available non-interactive checks and
diagnostics include:

    build/Sawer.exe --version
    build/Sawer.exe --third-party-notices
    build/Sawer.exe --smoke-test
    build/Sawer.exe --gpu-info
    build/Sawer.exe --render-test
    build/Sawer.exe --export-diagnostics diagnostics.log

`--third-party-notices` prints the complete license text embedded in the
executable; the same text is available from the toolbar's About button.
`--smoke-test` creates a hidden SDL window and exits. `--gpu-info` prints the
selected GPU backend, adapter, and driver version. `--render-test` presents
three frames before exiting. `--export-diagnostics` exports the local log and
current GPU startup diagnostics without opening a visible window.

## Package

After a Release build, create a portable archive through Meson:

    meson compile -C build package

The archive contains the statically linked `Sawer.exe`, this README, and
[third-party notices](THIRD_PARTY_NOTICES.md). Extract it anywhere and run
`Sawer.exe`; no installer is required. Linux builds produce an equivalent
relocatable `.tar.gz` archive.

Official Windows releases publish `Sawer-Setup.exe` as the recommended
one-click installer and `Sawer-Portable.exe` as the no-install alternative.
Both come from the same tested executable and include SHA-256 checksums and
GitHub build-provenance attestations. Maintainer steps are documented in
[docs/releasing.md](docs/releasing.md).

## Development

The project uses C++20, Meson, Ninja, SDL3, SDL GPU, SDL_ttf, and
nlohmann/json. Production code is organised under `src/`; automated unit and
application checks are under `tests/`. `meson test` runs the default test set;
performance regression checks are defined in Meson’s `performance` suite.
