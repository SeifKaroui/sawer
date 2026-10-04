<p align="center">
  <img src="assets/banner.png" alt="Sawer banner" width="100%">
</p>

# Sawer

Sawer is a fast, native whiteboard for Windows and Linux. Draw, sketch, and
organize ideas on a large canvas, with each board saved as one portable
`.sawer` file.

> Sawer 0.9.0 is prerelease software.

## Features

- **Simple.** A quiet interface and a small set of drawing tools keep the
  focus on your ideas.
- **Fast.** Native rendering keeps drawing, panning, and zooming responsive
  on large boards.
- **Offline.** Work locally without an internet connection, account, cloud
  service, or telemetry.
- **Portable.** Each board is one self-contained `.sawer` file, including
  pasted images. Save it anywhere, then move, back up, or share it.

## Download

Find downloads in the [v0.9.0 release](https://github.com/SeifKaroui/sawer/releases/tag/v0.9.0).
Both platforms target x64.

<details>
<summary><strong>Windows</strong></summary>

### Setup installer (recommended)

Download **sawer-v0.9.0-windows-x64-setup.exe**, run it, and choose **Install**. It installs for
your Windows account without administrator access, adds shortcuts, and
registers `.sawer` files so boards open directly from File Explorer.

### Portable executable

Download **sawer-v0.9.0-windows-x64-portable.exe** and run it from any folder. No installation or
companion files are needed.

Release checksums are in `sawer-v0.9.0-windows-x64-sha256sums.txt`. Windows downloads are not yet
code-signed, so SmartScreen may show a warning.

</details>

<details>
<summary><strong>Linux</strong></summary>

### Standalone binary (recommended)

Download **sawer-v0.9.0-linux-x64**, make it executable, and launch it:

```sh
chmod +x sawer-v0.9.0-linux-x64
./sawer-v0.9.0-linux-x64
```

SDL and application assets are embedded. A glibc-based distribution,
compatible system C++ runtime libraries, and a working Vulkan driver are required.

### AppImage

**sawer-v0.9.0-linux-x64.AppImage** bundles the C++ runtime libraries:

```sh
chmod +x sawer-v0.9.0-linux-x64.AppImage
./sawer-v0.9.0-linux-x64.AppImage
```

If FUSE mounting is unavailable, add `--appimage-extract-and-run` when launching.

### Flatpak

With Flatpak installed, install **sawer-v0.9.0-linux-x64.flatpak** and launch Sawer:

```sh
flatpak install --user ./sawer-v0.9.0-linux-x64.flatpak
flatpak run io.sawer.app
```

The runtime may download on first installation. Board access defaults to
Documents; see [folder permissions](docs/releasing.md#flatpak-packaging-and-installation)
for other locations.

Release checksums are in `sawer-v0.9.0-linux-x64-sha256sums.txt`.

</details>

## Build and run

Requires a C++20 compiler, Meson 1.7+, Ninja, Python 3.10+, and CMake 3.28+.
The first build downloads pinned dependencies and fonts. Linux development
packages are listed in the
[build workflow](https://github.com/SeifKaroui/sawer/blob/main/.github/workflows/linux-release.yml).

Build Release on either platform:

```sh
meson setup build --buildtype=release
meson compile -C build
```

If `build/` is already configured, add `--wipe` to the setup command.

Run on Windows:

```powershell
.\build\Sawer.exe
```

Run on Linux:

```sh
./build/Sawer
```

## Controls

| Action | Control |
| --- | --- |
| Draw | Left-drag with the current tool |
| Pencil / Line / Rectangle / Ellipse | `P` / `L` / `R` / `E` |
| Select / Hand | `V` / `H` |
| Pan | Hand tool, middle-drag, or `Space` + left-drag |
| Zoom / reset zoom | Mouse wheel / `Ctrl+0` |
| Constrain angles or proportions | Hold `Shift` while drawing |
| Shape fill / color / width | `F` / `1`–`7` / `[` and `]` |
| Move or resize | Drag selected objects or their handles |
| Select all / nudge | `Ctrl+A` / arrow keys (`Shift` for a larger step) |
| Copy / Cut / Paste | `Ctrl+C` / `Ctrl+X` / `Ctrl+V` |
| Duplicate / delete | `Ctrl+D` / `Delete` |
| Undo / redo | `Ctrl+Z` / `Ctrl+Shift+Z` or `Ctrl+Y` |
| New / Open / Save / Save As | `Ctrl+N` / `Ctrl+O` / `Ctrl+S` / `Ctrl+Shift+S` |
| Rename / cancel / theme | `F2` / `Escape` / `T` |

Hover toolbar controls for their purpose and shortcuts.

## Documentation and feedback

[File format](docs/file-format.md) · [Rendering](docs/rendering-performance.md) ·
[Board loading](docs/board-loading-performance.md) · [Release builds](docs/releasing.md) ·
[Report an issue](https://github.com/SeifKaroui/sawer/issues)

## License

Sawer is proprietary software. Dependency licenses are listed in
[Third-party notices](THIRD_PARTY_NOTICES.md).
