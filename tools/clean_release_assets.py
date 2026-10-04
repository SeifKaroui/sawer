"""Trim an existing release to packages and checksums, preserving package bytes."""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

from publish_release import download_names, gh, sha256


def release_layout(tag: str) -> tuple[set[str], set[str], set[str]]:
    if re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag) is None:
        raise ValueError("expected a vX.Y.Z release tag")
    packages = download_names(tag, "windows") | download_names(tag, "linux")
    checksums = {f"sawer-{tag}-{platform}-x64-sha256sums.txt" for platform in ("windows", "linux")}
    reports = {f"sawer-{tag}-{platform}-x64-third-party-notices.md" for platform in ("windows", "linux")}
    reports |= {f"sawer-{tag}-linux-x64{suffix}" for suffix in ("-dependencies.txt", "-flatpak-runtime.txt")}
    return packages, checksums, reports


def snapshot(repository: str, tag: str) -> dict:
    if re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repository) is None:
        raise ValueError("expected an owner/repository name")
    packages, checksums, reports = release_layout(tag)
    release = json.loads(gh("api", f"repos/{repository}/releases/tags/{tag}"))
    names = [asset["name"] for asset in release["assets"]]
    if release["tag_name"] != tag or release["draft"] or release.get("immutable"):
        raise ValueError("expected an editable published release")
    if len(set(names)) != len(names) or not (packages | checksums) <= set(names):
        raise ValueError("missing or duplicate release packages/checksums")
    if set(names) - packages - checksums - reports:
        raise ValueError("unexpected release assets; refusing cleanup")
    return release


def fingerprint(release: dict) -> dict:
    return {asset["name"]: (asset["id"], asset["size"], asset["digest"])
            for asset in release["assets"]}


def verify_backup(release: dict, directory: Path) -> None:
    for asset in release["assets"]:
        path = directory / asset["name"]
        if path.is_symlink() or not path.is_file() or path.stat().st_size != asset["size"]:
            raise ValueError("incomplete release backup")
        if "sha256:" + sha256(path) != asset["digest"]:
            raise ValueError("release backup checksum mismatch")


def prepare(repository: str, tag: str, directory: Path) -> None:
    release = snapshot(repository, tag)
    directory.mkdir(parents=True, exist_ok=False)
    gh("release", "download", tag, "--repo", repository, "--dir", str(directory),
       "--pattern", f"sawer-{tag}-*")
    verify_backup(release, directory)
    (directory / "release.json").write_text(json.dumps(release), encoding="utf-8")
    print(f"Backed up and verified {len(release['assets'])} assets for {tag}")


def apply(repository: str, tag: str, directory: Path) -> None:
    saved = json.loads((directory / "release.json").read_text(encoding="utf-8"))
    current = snapshot(repository, tag)
    if saved["id"] != current["id"] or fingerprint(saved) != fingerprint(current):
        raise ValueError("release changed after backup; refusing cleanup")
    verify_backup(saved, directory)
    packages, checksums, reports = release_layout(tag)
    output = directory / "public-checksums"
    output.mkdir(exist_ok=True)
    for platform in ("windows", "linux"):
        names = download_names(tag, platform)
        manifest = output / f"sawer-{tag}-{platform}-x64-sha256sums.txt"
        manifest.write_text("".join(f"{sha256(directory / name)}  {name}\n" for name in sorted(names)),
                            encoding="utf-8", newline="\n")
        if manifest.read_bytes() != (directory / manifest.name).read_bytes():
            gh("release", "upload", tag, "--repo", repository, "--clobber", str(manifest))
    for asset in current["assets"]:
        if asset["name"] in reports:
            gh("api", "--method", "DELETE", f"repos/{repository}/releases/assets/{asset['id']}")
    updated = snapshot(repository, tag)
    after = fingerprint(updated)
    before = fingerprint(saved)
    if set(after) != packages | checksums or any(after[name] != before[name] for name in packages):
        raise ValueError("release cleanup did not preserve the expected packages")
    for name in checksums:
        if after[name][2] != "sha256:" + sha256(output / name):
            raise ValueError("published checksum manifest mismatch")
    print(f"Verified {tag}: five original packages and two checksum files")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", required=True)
    parser.add_argument("--tag", required=True)
    parser.add_argument("--backup", type=Path, required=True)
    parser.add_argument("--apply", action="store_true")
    args = parser.parse_args()
    (apply if args.apply else prepare)(args.repository, args.tag, args.backup)
