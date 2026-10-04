"""Publish one verified, complete Windows/Linux release through GitHub CLI."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_assets(tag: str, platform: str, directory: Path) -> list[Path]:
    if re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag) is None:
        raise ValueError("expected a vX.Y.Z release tag")
    if platform == "windows":
        suffixes = ["-setup.exe", "-portable.exe", "-third-party-notices.md"]
    elif platform == "linux":
        suffixes = ["", ".AppImage", ".flatpak", "-third-party-notices.md",
                    "-dependencies.txt", "-flatpak-runtime.txt"]
    else:
        raise ValueError("unknown release platform")
    stem = f"sawer-{tag}-{platform}-x64"
    names = {stem + suffix for suffix in suffixes}
    manifest = directory / (stem + "-sha256sums.txt")
    expected = names | {manifest.name}
    if {path.name for path in directory.iterdir()} != expected:
        raise ValueError(f"incomplete or unexpected {platform} release assets")
    for name in expected:
        path = directory / name
        if path.is_symlink() or not path.is_file() or path.stat().st_size == 0:
            raise ValueError(f"invalid release asset: {name}")
    hashes: dict[str, str] = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        entry = re.fullmatch(r"([0-9a-fA-F]{64}) [ *](.+)", line)
        if entry is None:
            raise ValueError("malformed release checksum entry")
        digest, name = entry.groups()
        if name not in names or name in hashes:
            raise ValueError(f"unexpected or duplicate checksum entry: {name}")
        hashes[name] = digest.lower()
    if set(hashes) != names:
        raise ValueError(f"missing {platform} release checksums")
    for name, expected_hash in hashes.items():
        if sha256(directory / name) != expected_hash:
            raise ValueError(f"release checksum mismatch: {name}")
    return [directory / name for name in sorted(expected)]


def download_names(tag: str, platform: str) -> set[str]:
    suffixes = ["-setup.exe", "-portable.exe"] if platform == "windows" else ["", ".AppImage", ".flatpak"]
    return {f"sawer-{tag}-{platform}-x64{suffix}" for suffix in suffixes}


def prepare_downloads(tag: str, windows: Path, linux: Path, output: Path) -> list[Path]:
    # Reports remain in the CI artifacts; public checksums cover packages only.
    assets = []
    for platform, directory in (("windows", windows), ("linux", linux)):
        validate_assets(tag, platform, directory)
        names = download_names(tag, platform)
        for name in sorted(names):
            destination = output / name
            shutil.copyfile(directory / name, destination)
            assets.append(destination)
        manifest = output / f"sawer-{tag}-{platform}-x64-sha256sums.txt"
        manifest.write_text("".join(f"{sha256(output / name)}  {name}\n" for name in sorted(names)),
                            encoding="utf-8", newline="\n")
        assets.append(manifest)
    return assets


def gh(*arguments: str) -> str:
    return subprocess.run(["gh", *arguments], check=True, capture_output=True,
                          text=True, timeout=180).stdout


def publish(repository: str, tag: str, commit: str,
            windows: Path, linux: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="sawer-release-assets-") as temporary:
        assets = prepare_downloads(tag, windows, linux, Path(temporary))
        publish_downloads(repository, tag, commit, assets)


def publish_downloads(repository: str, tag: str, commit: str, assets: list[Path]) -> None:
    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) is None:
        raise ValueError("expected an owner/repository name")
    if re.fullmatch(r"[0-9a-fA-F]{40}", commit) is None:
        raise ValueError("expected a full release commit SHA")
    endpoint = f"repos/{repository}"
    target = json.loads(gh("api", f"{endpoint}/git/ref/tags/{tag}"))["object"]
    for _ in range(8):
        if target["type"] != "tag":
            break
        target = json.loads(gh("api", f"{endpoint}/git/tags/{target['sha']}"))["object"]
    if target["type"] != "commit" or target["sha"].lower() != commit.lower():
        raise ValueError("release tag does not point to the tested commit")
    try:
        release = json.loads(gh("api", f"{endpoint}/releases/tags/{tag}"))
    except subprocess.CalledProcessError as error:
        if "HTTP 404" not in (error.stderr or ""):
            raise
        release = None
    if release is not None:
        if not release["draft"]:
            raise ValueError("refusing to overwrite a published release")
        if {asset["name"] for asset in release["assets"]} - {path.name for path in assets}:
            raise ValueError("existing draft contains unexpected assets")
    else:
        arguments = ["release", "create", tag, "--repo", repository,
                     "--verify-tag", "--draft", "--generate-notes", "--title", f"Sawer {tag[1:]}"]
        if tag.startswith("v0."):
            arguments.append("--prerelease")
        gh(*arguments)
    gh("release", "upload", tag, "--repo", repository, "--clobber",
       *(str(path.resolve()) for path in assets))
    # Download the actual uploaded files before making the release public.
    with tempfile.TemporaryDirectory(prefix="sawer-release-verification-") as temporary:
        gh("release", "download", tag, "--repo", repository, "--dir", temporary,
           "--pattern", f"sawer-{tag}-*")
        downloaded = Path(temporary)
        if {path.name for path in downloaded.iterdir()} != {path.name for path in assets}:
            raise ValueError("uploaded release contains an incomplete or unexpected file set")
        for path in assets:
            if sha256(downloaded / path.name) != sha256(path):
                raise ValueError(f"uploaded release differs from the tested artifact: {path.name}")
    gh("release", "edit", tag, "--repo", repository, "--draft=false",
       "--prerelease=" + str(tag.startswith("v0.")).lower())
    print(f"Published https://github.com/{repository}/releases/tag/{tag}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--windows", type=Path, required=True)
    parser.add_argument("--linux", type=Path, required=True)
    arguments = parser.parse_args()
    publish(arguments.repository, arguments.tag, arguments.commit,
            arguments.windows, arguments.linux)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print(f"Release publication failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
