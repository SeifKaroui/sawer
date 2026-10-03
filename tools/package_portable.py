from __future__ import annotations

import argparse
import tarfile
import zipfile
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True)
    parser.add_argument("--readme", required=True)
    parser.add_argument("--banner", required=True)
    parser.add_argument("--notices", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--documentation", nargs="*", type=Path, default=[])
    arguments = parser.parse_args()

    files = [
        (Path(arguments.executable), Path(arguments.executable).name),
        (Path(arguments.readme), "README.md"),
        (Path(arguments.banner), "assets/banner.png"),
        (Path(arguments.notices), "THIRD_PARTY_NOTICES.md"),
    ]
    source_root = (arguments.source_root or Path(arguments.readme).parent).resolve()
    for document in arguments.documentation:
        relative = document.resolve().relative_to(source_root)
        files.append((document, relative.as_posix()))
    output = Path(arguments.output)
    root = arguments.root

    if output.suffix == ".zip":
        with zipfile.ZipFile(
            output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as archive:
            for path, relative in files:
                archive.write(path, f"{root}/{relative}")
    else:
        with tarfile.open(output, "w:gz", compresslevel=9) as archive:
            for path, relative in files:
                archive.add(path, arcname=f"{root}/{relative}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
