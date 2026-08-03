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
    arguments = parser.parse_args()

    files = [
        Path(arguments.executable),
        Path(arguments.readme),
        Path(arguments.banner),
        Path(arguments.notices),
    ]
    output = Path(arguments.output)
    root = arguments.root

    if output.suffix == ".zip":
        with zipfile.ZipFile(
            output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9
        ) as archive:
            for path in files:
                archive.write(path, f"{root}/{path.name}")
    else:
        with tarfile.open(output, "w:gz", compresslevel=9) as archive:
            for path in files:
                archive.add(path, arcname=f"{root}/{path.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
