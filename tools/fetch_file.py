from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import urllib.request
from pathlib import Path


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--cache", default="")
    arguments = parser.parse_args()

    output = Path(arguments.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    cached = Path(arguments.cache) / output.name if arguments.cache else None

    if cached is not None and cached.is_file():
        if digest(cached) != arguments.sha256:
            raise RuntimeError(f"cached file has the wrong SHA-256: {cached}")
        shutil.copyfile(cached, output)
    else:
        temporary = output.with_suffix(output.suffix + ".download")
        try:
            urllib.request.urlretrieve(arguments.url, temporary)
            if digest(temporary) != arguments.sha256:
                raise RuntimeError(f"download has the wrong SHA-256: {arguments.url}")
            temporary.replace(output)
        finally:
            temporary.unlink(missing_ok=True)

    if digest(output) != arguments.sha256:
        raise RuntimeError(f"output has the wrong SHA-256: {output}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"font fetch failed: {error}", file=sys.stderr)
        raise SystemExit(1)
