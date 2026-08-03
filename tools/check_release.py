from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


VERSION_PATTERN = r"[0-9]+\.[0-9]+\.[0-9]+"


def require_version(path: Path, pattern: str, label: str) -> str:
    match = re.search(pattern, path.read_text(encoding="utf-8"))
    if match is None:
        raise ValueError(f"could not find {label} version in {path}")
    return match.group(1)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, default=Path.cwd())
    parser.add_argument("--tag")
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--print-version", action="store_true")
    arguments = parser.parse_args()
    root = arguments.source_root.resolve()

    versions = {
        "Meson": require_version(
            root / "meson.build",
            rf"version:\s*'({VERSION_PATTERN})'",
            "Meson",
        ),
        "BuildInfo": require_version(
            root / "src/core/BuildInfo.hpp",
            rf'version\{{"({VERSION_PATTERN})"\}}',
            "BuildInfo",
        ),
        "BuildInfo test": require_version(
            root / "tests/core/BuildInfoTests.cpp",
            rf'string_view\{{"({VERSION_PATTERN})"\}}',
            "BuildInfo test",
        ),
    }
    expected = versions["Meson"]
    mismatches = [
        f"{label}={version}"
        for label, version in versions.items()
        if version != expected
    ]

    if arguments.tag is not None:
        match = re.fullmatch(rf"v({VERSION_PATTERN})", arguments.tag)
        if match is None:
            raise ValueError(
                f"release tag must use vX.Y.Z, got {arguments.tag!r}"
            )
        if match.group(1) != expected:
            mismatches.append(f"tag={match.group(1)}")

    if arguments.executable is not None:
        executable = arguments.executable.resolve()
        completed = subprocess.run(
            [str(executable), "--version"],
            check=True,
            capture_output=True,
            text=True,
        )
        runtime = completed.stdout.strip()
        wanted = f"Sawer {expected}"
        if runtime != wanted:
            mismatches.append(f"runtime={runtime!r}, expected={wanted!r}")

        notices = subprocess.run(
            [str(executable), "--third-party-notices"],
            check=True,
            capture_output=True,
        ).stdout
        source_notices = (root / "THIRD_PARTY_NOTICES.md").read_bytes()
        if notices != source_notices:
            mismatches.append("embedded third-party notices differ from source")

    if mismatches:
        raise ValueError(
            f"release version {expected} is inconsistent: " + ", ".join(mismatches)
        )

    if arguments.print_version:
        print(expected)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"release check failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
