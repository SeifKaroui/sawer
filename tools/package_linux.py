"""Stage an x86_64 AppDir, keeping host graphics libraries out of the payload."""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


# SDL loads display libraries and the Vulkan driver from the host at runtime.
SYSTEM_LIBRARIES = {
    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1",
    "ld-linux-x86-64.so.2",
}
BUNDLED_LIBRARIES = {"libstdc++.so.6", "libgcc_s.so.1", "libatomic.so.1"}


def check_executable(executable: Path) -> None:
    with executable.open("rb") as source:
        header = source.read(20)
    if (len(header) != 20 or header[:6] != b"\x7fELF\x02\x01"
            or int.from_bytes(header[18:20], "little") != 62):
        raise ValueError("expected a little-endian Linux x86_64 ELF executable")


def parse_dependencies(output: str) -> dict[str, Path]:
    libraries: dict[str, Path] = {}
    for line in output.splitlines():
        line = line.strip()
        if not line or line.startswith("linux-vdso.so."):
            continue
        match = re.fullmatch(r"(\S+) => (.+) \(0x[0-9a-fA-F]+\)", line)
        if match is not None:
            name, location = match.groups()
        else:
            loader = re.fullmatch(r"(/.+/ld-linux-x86-64\.so\.2) \(0x[0-9a-fA-F]+\)", line)
            if loader is None:
                raise ValueError(f"unresolved or unrecognized ELF dependency: {line}")
            location = loader.group(1)
            name = "ld-linux-x86-64.so.2"
        if name not in SYSTEM_LIBRARIES | BUNDLED_LIBRARIES:
            raise ValueError(f"unexpected shared dependency: {name}; SDL must be static")
        if name in libraries:
            raise ValueError(f"duplicate ELF dependency: {name}")
        path = Path(location)
        if not path.is_absolute() or not path.is_file():
            raise ValueError(f"missing dependency: {name} at {path}")
        libraries[name] = path
    if "libc.so.6" not in libraries:
        raise ValueError("could not identify the executable's glibc dependencies")
    return libraries


def stage_appdir(source: Path, executable: Path, appdir: Path,
                 libraries: dict[str, Path]) -> None:
    # Refuse stale payloads rather than deleting a caller-supplied directory.
    appdir.mkdir(parents=True, exist_ok=False)
    binary = appdir / "usr/bin/Sawer"
    binary.parent.mkdir(parents=True)
    shutil.copyfile(executable, binary)
    binary.chmod(0o755)
    library_dir = appdir / "usr/lib"
    library_dir.mkdir(parents=True)
    for name, path in libraries.items():
        if name in BUNDLED_LIBRARIES:
            # Dereference SONAME links: no link may escape the AppDir.
            shutil.copyfile(path, library_dir / name)
    launcher = appdir / "AppRun"
    shutil.copyfile(source / "packaging/linux/AppRun", launcher)
    launcher.chmod(0o755)
    shutil.copyfile(source / "packaging/linux/io.sawer.app.desktop",
                    appdir / "io.sawer.app.desktop")
    shutil.copyfile(source / "assets/logo.png", appdir / "io.sawer.app.png")
    shutil.copyfile(source / "assets/logo.png", appdir / ".DirIcon")
    documentation = appdir / "usr/share/doc/Sawer"
    documentation.mkdir(parents=True)
    for name in ("THIRD_PARTY_NOTICES.md", "README.md"):
        shutil.copyfile(source / name, documentation / name)
    shutil.copytree(source / "docs", documentation / "docs")
    (documentation / "assets/shaders").mkdir(parents=True)
    shutil.copyfile(source / "assets/banner.png", documentation / "assets/banner.png")
    shutil.copyfile(source / "assets/shaders/README.md",
                    documentation / "assets/shaders/README.md")
    (documentation / "tools").mkdir()
    shutil.copyfile(source / "tools/compile_shaders.py",
                    documentation / "tools/compile_shaders.py")
    shutil.copyfile(source / "packaging/linux/AppImage-runtime-LICENSE.txt",
                    documentation / "AppImage-runtime-LICENSE.txt")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=Path.cwd())
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--appdir", type=Path, required=True)
    arguments = parser.parse_args()
    executable = arguments.executable.resolve()
    check_executable(executable)
    output = subprocess.run(["ldd", str(executable)], check=True,
                            capture_output=True, text=True, timeout=30).stdout
    libraries = parse_dependencies(output)
    stage_appdir(arguments.source_root.resolve(), executable,
                 arguments.appdir.resolve(), libraries)
    for name in sorted(libraries.keys() & BUNDLED_LIBRARIES):
        print(f"Bundled compiler runtime: {name}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Linux packaging failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error