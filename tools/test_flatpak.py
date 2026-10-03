"""Check the installed Flatpak bundle's contents and sandboxed startup."""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

from check_release import VERSION_PATTERN, require_version
from package_flatpak import APP_ID
from test_linux_display import run_session

# Driver paths refer to the Freedesktop runtime, never the host's Vulkan ICD.
RENDER_SCRIPT = r'''
unset VK_DRIVER_FILES VK_ADD_DRIVER_FILES VK_ICD_FILENAMES
for directory in /usr/lib/x86_64-linux-gnu/GL/default/share/vulkan/icd.d /usr/share/vulkan/icd.d; do
    for driver in "$directory"/lvp_icd*.json; do
        if [ -f "$driver" ]; then
            export VK_ICD_FILENAMES="$driver"
            exec /app/bin/Sawer --render-test
        fi
    done
done
echo 'No Mesa lavapipe Vulkan ICD in the Flatpak runtime' >&2
exit 1
'''


def flatpak_command() -> list[str]:
    return ["flatpak", "run", "--user", "--arch=x86_64", "--branch=stable"]


def check_metadata(source: Path, executable: Path) -> None:
    location = Path(subprocess.check_output([
        "flatpak", "info", "--user", "--arch=x86_64", "--show-location",
        f"{APP_ID}//stable",
    ], text=True, timeout=60).strip())
    installed = location / "files"
    if (installed / "bin/Sawer").read_bytes() != executable.read_bytes():
        raise ValueError("installed Flatpak executable differs from the release payload")
    notices = (source / "THIRD_PARTY_NOTICES.md").read_bytes()
    if (installed / "share/doc/Sawer/THIRD_PARTY_NOTICES.md").read_bytes() != notices:
        raise ValueError("installed Flatpak documentation notices differ from source")
    version = require_version(source / "meson.build",
                              rf"version:\s*'({VERSION_PATTERN})'", "Meson")
    command = flatpak_command() + [APP_ID]
    actual = subprocess.check_output(command + ["--version"], timeout=60).decode().strip()
    if actual != f"Sawer {version}":
        raise ValueError(f"unexpected Flatpak version: {actual!r}")
    if subprocess.check_output(command + ["--third-party-notices"], timeout=60) != notices:
        raise ValueError("sandboxed Flatpak notices differ from source")
    print(f"Flatpak payload, version, and notices verified: {actual}", flush=True)


def render_command(backend: str) -> list[str]:
    return flatpak_command() + [
        f"--env=SDL_VIDEODRIVER={backend}", "--env=SDL_AUDIODRIVER=dummy",
        "--env=LIBGL_ALWAYS_SOFTWARE=1", "--command=sh", APP_ID,
        "-eu", "-c", RENDER_SCRIPT,
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path.cwd())
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--graphics", action="store_true")
    parser.add_argument("--log-dir", type=Path, default=Path("build/flatpak-logs"))
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Flatpak checks require Linux")
    # The display helper isolates XDG paths; retain the installed bundle location.
    data = Path(os.environ.get("XDG_DATA_HOME", str(Path.home() / ".local/share")))
    os.environ.setdefault("FLATPAK_USER_DIR", str((data / "flatpak").resolve()))
    check_metadata(args.source_root.resolve(), args.executable.resolve())
    if args.graphics:
        for backend in ("x11", "wayland"):
            result = run_session(backend, render_command(backend), args.log_dir)
            if result:
                return result
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Flatpak checks failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
