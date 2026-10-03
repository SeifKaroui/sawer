"""Select and verify the shared GCC release used by Windows and Linux CI."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIN = ROOT / "tools" / "gcc-toolchain.json"


def environment(platform: str) -> dict[str, str]:
    pin = json.loads(PIN.read_text(encoding="utf-8"))
    result = {"GCC_VERSION": pin["gcc_version"]}
    if platform == "linux":
        linux = pin["linux"]
        result.update({
            "CC": f"gcc-{linux['major']}",
            "CXX": f"g++-{linux['major']}",
            "GCC_MAJOR": linux["major"],
            "GCC_PACKAGE_VERSION": linux["package_version"],
            "GCC_REPOSITORY": linux["repository"],
        })
    else:
        directory = ROOT / ".cache" / "toolchains" / f"gcc-{pin['gcc_version']}"
        binary = directory / "mingw64" / "bin"
        result.update({
            "CC": (binary / "gcc.exe").as_posix(),
            "CXX": (binary / "g++.exe").as_posix(),
            "GCC_TOOLCHAIN_DIR": directory.as_posix(),
            "GCC_TOOLCHAIN_BIN": binary.as_posix(),
            "GCC_ARCHIVE_URL": pin["windows"]["url"],
            "GCC_ARCHIVE_SHA256": pin["windows"]["sha256"],
        })
    return result


def check() -> None:
    expected = json.loads(PIN.read_text(encoding="utf-8"))["gcc_version"]
    for variable in ("CC", "CXX"):
        compiler = os.environ[variable]
        version = subprocess.check_output(
            [compiler, "-dumpfullversion", "-dumpversion"], text=True
        ).strip()
        if version != expected:
            raise RuntimeError(f"{variable}: expected GCC {expected}, got {version!r}")
        target = subprocess.check_output([compiler, "-dumpmachine"], text=True).strip()
        if not target.startswith("x86_64-"):
            raise RuntimeError(f"{variable}: expected x86_64 target, got {target!r}")
        print(f"{variable}: GCC {version} ({target})", flush=True)
    # Compile the source that needs clock_cast before the full dependency build.
    subprocess.run([
        os.environ["CXX"], "-std=c++20", "-Wall", "-Wextra", "-Wpedantic",
        "-Werror", "-fsyntax-only", f"-I{ROOT / 'src'}",
        str(ROOT / "src" / "storage" / "BoardPath.cpp"),
    ], check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    configure = commands.add_parser("environment")
    configure.add_argument("platform", choices=("windows", "linux"))
    configure.add_argument("--github-env", type=Path, required=True)
    commands.add_parser("check")
    args = parser.parse_args()
    if args.command == "check":
        check()
    else:
        with args.github_env.open("a", encoding="utf-8", newline="\n") as destination:
            for name, value in environment(args.platform).items():
                destination.write(f"{name}={value}\n")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, KeyError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"GCC toolchain check failed: {error}", file=sys.stderr)
        raise SystemExit(1)
