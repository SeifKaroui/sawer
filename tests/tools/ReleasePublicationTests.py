"""Exercise publication guards without contacting GitHub or publishing a release."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE / "tools"))
import publish_release

TAG = "v0.9.0"
COMMIT = "a" * 40
TAG_OBJECT = "b" * 40


class ReleasePublicationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.assets = []
        for platform, suffixes in (
            ("windows", ("-setup.exe", "-portable.exe", "-third-party-notices.md")),
            ("linux", ("", ".AppImage", ".flatpak", "-third-party-notices.md",
                       "-dependencies.txt", "-flatpak-runtime.txt")),
        ):
            directory = self.root / platform
            directory.mkdir()
            stem = f"sawer-{TAG}-{platform}-x64"
            checksums = []
            for suffix in suffixes:
                path = directory / (stem + suffix)
                contents = (platform + suffix).encode()
                path.write_bytes(contents)
                checksums.append(hashlib.sha256(contents).hexdigest() + "  " + path.name)
                self.assets.append(path)
            manifest = directory / (stem + "-sha256sums.txt")
            manifest.write_text("\n".join(checksums) + "\n")
            self.assets.append(manifest)
        self.calls = []
        self.uploaded = {}
        self.release = None
        self.target_commit = COMMIT
        self.corrupt_download = False
        self.failed_upload = False
        self.denied_read = False

    def github(self, *arguments):
        self.calls.append(arguments)
        if arguments[0] == "api":
            if "/git/ref/tags/" in arguments[1]:
                return json.dumps({"object": {"type": "tag", "sha": TAG_OBJECT}})
            if "/git/tags/" in arguments[1]:
                return json.dumps({"object": {"type": "commit", "sha": self.target_commit}})
            if self.denied_read:
                raise subprocess.CalledProcessError(1, arguments, stderr="gh: Forbidden (HTTP 403)")
            if self.release is None:
                raise subprocess.CalledProcessError(1, arguments, stderr="gh: Not Found (HTTP 404)")
            return json.dumps(self.release)
        if arguments[:2] == ("release", "upload") and self.failed_upload:
            raise subprocess.CalledProcessError(1, arguments, stderr="upload failed")
        if arguments[:2] == ("release", "upload"):
            self.uploaded = {Path(name).name: Path(name).read_bytes()
                             for name in arguments[6:]}
        if arguments[:2] == ("release", "download"):
            directory = Path(arguments[arguments.index("--dir") + 1])
            for name, contents in self.uploaded.items():
                (directory / name).write_bytes(contents)
            if self.corrupt_download:
                (directory / self.assets[0].name).write_bytes(b"wrong upload")
        return ""

    def publish(self):
        with patch.object(publish_release, "gh", self.github):
            publish_release.publish("fixture/sawer", TAG, COMMIT,
                                    self.root / "windows", self.root / "linux")

    def assert_unpublished(self):
        self.assertFalse(any(call[:2] == ("release", "edit") for call in self.calls))

    def test_complete_release_is_verified_before_publication(self):
        self.publish()
        operations = [call[:2] for call in self.calls if call[0] == "release"]
        self.assertEqual(operations, [("release", "create"), ("release", "upload"),
                                      ("release", "download"), ("release", "edit")])
        creation = next(call for call in self.calls if call[:2] == ("release", "create"))
        self.assertIn("--verify-tag", creation)
        self.assertIn("--draft", creation)
        self.assertIn("--prerelease", creation)
        expected = publish_release.download_names(TAG, "windows") | publish_release.download_names(TAG, "linux")
        expected |= {f"sawer-{TAG}-{platform}-x64-sha256sums.txt" for platform in ("windows", "linux")}
        self.assertEqual(set(self.uploaded), expected)
        self.assertEqual(len(self.uploaded), 7)
        for platform in ("windows", "linux"):
            lines = self.uploaded[f"sawer-{TAG}-{platform}-x64-sha256sums.txt"].decode().splitlines()
            self.assertEqual(set(line[66:] for line in lines), publish_release.download_names(TAG, platform))
            for line in lines:
                self.assertEqual(line[:64], hashlib.sha256(self.uploaded[line[66:]]).hexdigest())
        for path in self.assets:
            if path.name in expected and not path.name.endswith("-sha256sums.txt"):
                self.assertEqual(self.uploaded[path.name], path.read_bytes())
        self.assertIn("--draft=false", self.calls[-1])
        self.assertIn("--prerelease=true", self.calls[-1])

    def test_draft_retry_uses_existing_release(self):
        self.release = {"draft": True, "assets": [{"name": self.assets[0].name}]}
        self.publish()
        self.assertFalse(any(call[:2] == ("release", "create") for call in self.calls))
        self.assertIn("--prerelease=true", self.calls[-1])

    def test_published_release_and_unrelated_draft_assets_are_preserved(self):
        for release in ({"draft": False, "assets": []},
                        {"draft": True, "assets": [{"name": "unrelated.zip"}]}):
            with self.subTest(release=release):
                self.release = release
                self.calls.clear()
                with self.assertRaises(ValueError):
                    self.publish()
                self.assertFalse(any(call[0] == "release" for call in self.calls))

    def test_tag_must_match_the_tested_commit(self):
        self.target_commit = "c" * 40
        with self.assertRaisesRegex(ValueError, "tested commit"):
            self.publish()
        self.assertFalse(any(call[0] == "release" for call in self.calls))

    def test_upload_failure_or_corrupt_download_leaves_release_unpublished(self):
        for failure in ("failed_upload", "corrupt_download"):
            with self.subTest(failure=failure):
                setattr(self, failure, True)
                self.calls.clear()
                with self.assertRaises((ValueError, subprocess.CalledProcessError)):
                    self.publish()
                self.assert_unpublished()
                setattr(self, failure, False)

    def test_permission_errors_do_not_create_a_release(self):
        self.denied_read = True
        with self.assertRaises(subprocess.CalledProcessError):
            self.publish()
        self.assertFalse(any(call[0] == "release" for call in self.calls))

    def test_incomplete_or_modified_payload_is_rejected_before_github_access(self):
        self.assets[0].write_bytes(b"modified")
        with self.assertRaisesRegex(ValueError, "checksum mismatch"):
            self.publish()
        self.assertEqual(self.calls, [])
        self.assets[0].unlink()
        with self.assertRaisesRegex(ValueError, "release assets"):
            self.publish()
        self.assertEqual(self.calls, [])

    def test_manifest_rejects_missing_duplicate_and_unsafe_entries(self):
        manifest = self.root / "windows" / "sawer-v0.9.0-windows-x64-sha256sums.txt"
        original = manifest.read_text()
        malformed = [original.splitlines()[0] + "\n",
                     original + original.splitlines()[0] + "\n",
                     original + "0" * 64 + "  ../outside.exe\n"]
        for contents in malformed:
            with self.subTest(contents=contents):
                manifest.write_text(contents)
                with self.assertRaises(ValueError):
                    self.publish()
                self.assertEqual(self.calls, [])


if __name__ == "__main__":
    unittest.main()
