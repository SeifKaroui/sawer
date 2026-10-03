"""Stage the verified Linux payload for Flatpak without rebuilding Sawer."""
from __future__ import annotations

import argparse
from datetime import date
import re
import shutil
import struct
import sys
from pathlib import Path

from package_linux import check_executable

APP_ID = "io.sawer.app"


def icon_png(source: Path) -> bytes:
    # Reuse the existing 256px PNG from the application icon, without resampling.
    icon = (source / "assets/windows/Sawer.ico").read_bytes()
    if icon[:4] != b"\x00\x00\x01\x00":
        raise ValueError("expected the existing ICO application icon")
    count = int.from_bytes(icon[4:6], "little")
    for index in range(count):
        width, height, _, _, _, _, size, offset = struct.unpack_from(
            "<BBBBHHII", icon, 6 + index * 16)
        if width == 0 and height == 0:
            png = icon[offset:offset + size]
            if (len(png) != size or png[:8] != b"\x89PNG\r\n\x1a\n"
                    or struct.unpack_from(">II", png, 16) != (256, 256)):
                raise ValueError("expected a complete 256px PNG application icon")
            return png
    raise ValueError("application icon has no 256px PNG")


def stage_flatpak(source: Path, appdir: Path, output: Path,
                  version: str, release_date: str) -> None:
    if re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) is None:
        raise ValueError("expected an X.Y.Z release version")
    date.fromisoformat(release_date)
    binary = appdir / "usr/bin/Sawer"
    check_executable(binary)
    png = icon_png(source)
    # Refuse stale output instead of deleting a caller-supplied directory.
    output.mkdir(parents=True, exist_ok=False)
    payload = output / "payload"
    shutil.copytree(appdir / "usr", payload)
    (payload / "bin/Sawer").chmod(0o755)
    packaging = source / "packaging/linux"
    shutil.copyfile(packaging / f"{APP_ID}.json", output / f"{APP_ID}.json")
    for directory, filename in (
        ("applications", f"{APP_ID}.desktop"),
        ("mime/packages", f"{APP_ID}.xml"),
    ):
        destination = payload / "share" / directory / filename
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(packaging / filename, destination)
    metainfo = payload / "share/metainfo" / f"{APP_ID}.metainfo.xml"
    metainfo.parent.mkdir(parents=True, exist_ok=True)
    template = (packaging / f"{APP_ID}.metainfo.xml.in").read_text(encoding="utf-8")
    metainfo.write_text(template.replace("@VERSION@", version).replace(
        "@DATE@", release_date), encoding="utf-8")
    icon = payload / "share/icons/hicolor/256x256/apps" / f"{APP_ID}.png"
    icon.parent.mkdir(parents=True, exist_ok=True)
    icon.write_bytes(png)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path.cwd())
    parser.add_argument("--appdir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--release-date", required=True)
    args = parser.parse_args()
    stage_flatpak(args.source_root.resolve(), args.appdir.resolve(),
                  args.output.resolve(), args.version, args.release_date)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, struct.error) as error:
        print(f"Flatpak staging failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
