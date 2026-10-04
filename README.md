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

<p align="center">
  <img src="https://raw.githubusercontent.com/SeifKaroui/sawer/main/assets/demo.gif" alt="Sawer drawing and board navigation demo" width="100%">
</p>

## Download

Open the [v0.9.0 release page](https://github.com/SeifKaroui/sawer/releases/tag/v0.9.0)
and expand **Assets** to find the files below. Choose one package for your
platform; all downloads target x64.

<details>
<summary><strong>Windows</strong></summary>

**Windows security warning:** The installer and portable executable are not yet
code-signed, so SmartScreen may show **Windows protected your PC** before launch.
This warning can appear for legitimate unsigned apps and does not by itself mean
Sawer is harmful. Download from this repository's official release page and verify
the release checksum, then choose **More info → Run anyway** if this prompt appears.

<details>
<summary><strong>Setup installer (recommended)</strong></summary>

1. Download **sawer-v0.9.0-windows-x64-setup.exe** from the
   [release page](https://github.com/SeifKaroui/sawer/releases/tag/v0.9.0).
2. Open the downloaded file and choose **Install**.
3. Launch **Sawer** from the Start menu or desktop shortcut.

Setup installs for your Windows account without administrator access and
lets you open `.sawer` boards directly from File Explorer.

</details>

<details>
<summary><strong>Portable executable</strong></summary>

1. Download **sawer-v0.9.0-windows-x64-portable.exe** from the
   [release page](https://github.com/SeifKaroui/sawer/releases/tag/v0.9.0).
2. Move it to any folder and double-click it to launch Sawer.

No installation or companion files are needed.

</details>

Release checksums are in `sawer-v0.9.0-windows-x64-sha256sums.txt`.

</details>

<details>
<summary><strong>Linux</strong></summary>

<details>
<summary><strong>Flatpak (recommended)</strong></summary>

1. Install Flatpak using your distribution's software manager if needed.
2. Download **sawer-v0.9.0-linux-x64.flatpak** from the
   [release page](https://github.com/SeifKaroui/sawer/releases/tag/v0.9.0).
3. Open a terminal in the folder containing the download, then install and
   launch Sawer:

```sh
flatpak install --user ./sawer-v0.9.0-linux-x64.flatpak
flatpak run io.sawer.app
```

Accept the installation prompts; the required runtime may download the first
time. After installation, you can also launch **Sawer** from your app menu.
Boards are accessible in **Documents** by default; see
[folder permissions](docs/releasing.md#flatpak-packaging-and-installation)
to allow other locations.

</details>

<details>
<summary><strong>AppImage</strong></summary>

1. Download **sawer-v0.9.0-linux-x64.AppImage** from the
   [release page](https://github.com/SeifKaroui/sawer/releases/tag/v0.9.0).
2. Open a terminal in the download folder and run:

```sh
chmod +x sawer-v0.9.0-linux-x64.AppImage
./sawer-v0.9.0-linux-x64.AppImage
```

AppImage runs without installation and bundles the C++ runtime libraries.
If it reports a FUSE error, launch it with:

```sh
./sawer-v0.9.0-linux-x64.AppImage --appimage-extract-and-run
```

</details>

<details>
<summary><strong>Standalone binary</strong></summary>

1. Download **sawer-v0.9.0-linux-x64** from the
   [release page](https://github.com/SeifKaroui/sawer/releases/tag/v0.9.0).
2. Open a terminal in the download folder and run:

```sh
chmod +x sawer-v0.9.0-linux-x64
./sawer-v0.9.0-linux-x64
```

The binary runs without installation. It needs a glibc-based distribution,
compatible system C++ runtime libraries, and a working Vulkan driver. Choose
Flatpak or AppImage if your system is missing the required C++ runtime.

</details>

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
