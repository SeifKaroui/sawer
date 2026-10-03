from __future__ import annotations

import argparse
import hashlib
import http.client
import shutil
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path


def digest(path: Path) -> str:
    checksum = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def download(url: str, temporary: Path) -> None:
    for attempt in range(1, 4):
        try:
            with urllib.request.urlopen(url, timeout=30) as source:
                expected_bytes = source.headers.get("Content-Length")
                with temporary.open("wb") as destination:
                    shutil.copyfileobj(source, destination)
            if expected_bytes is not None:
                actual_bytes = temporary.stat().st_size
                if actual_bytes != int(expected_bytes):
                    raise urllib.error.ContentTooShortError(
                        f"retrieval incomplete: got {actual_bytes} "
                        f"out of {expected_bytes} bytes", None)
            return
        except (OSError, http.client.HTTPException) as error:
            temporary.unlink(missing_ok=True)
            if attempt == 3:
                raise
            print(
                f"font download interrupted ({attempt}/3): {error}; retrying",
                file=sys.stderr,
                flush=True,
            )
            time.sleep(attempt)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--url", required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--cache", default="")
    parser.add_argument("--archive-member", default="")
    arguments = parser.parse_args()

    output = Path(arguments.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.is_file() and digest(output) == arguments.sha256:
        return 0
    cached = Path(arguments.cache) / output.name if arguments.cache else None
    temporary = output.with_suffix(output.suffix + ".download")
    staged = output.with_suffix(output.suffix + ".tmp")

    try:
        if cached is not None and cached.is_file():
            if digest(cached) != arguments.sha256:
                raise RuntimeError(f"cached file has the wrong SHA-256: {cached}")
            shutil.copyfile(cached, staged)
        else:
            download(arguments.url, temporary)
            if arguments.archive_member:
                with zipfile.ZipFile(temporary) as archive:
                    with archive.open(arguments.archive_member) as source:
                        with staged.open("wb") as destination:
                            shutil.copyfileobj(source, destination)
            else:
                temporary.replace(staged)
        if digest(staged) != arguments.sha256:
            raise RuntimeError(f"download has the wrong SHA-256: {output}")
        staged.replace(output)
    finally:
        temporary.unlink(missing_ok=True)
        staged.unlink(missing_ok=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"font fetch failed: {error}", file=sys.stderr)
        raise SystemExit(1)
