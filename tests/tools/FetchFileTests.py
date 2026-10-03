from __future__ import annotations

import hashlib
import importlib.util
import io
import os
import tempfile
import unittest
import urllib.error
import zipfile
from pathlib import Path
from unittest.mock import patch


spec = importlib.util.spec_from_file_location(
    "fetch_file", Path(__file__).resolve().parents[2] / "tools" / "fetch_file.py")
fetch_file = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch_file)


class Response(io.BytesIO):
    def __init__(self, payload: bytes, expected_bytes: int | None = None):
        super().__init__(payload)
        self.headers = {"Content-Length": str(
            len(payload) if expected_bytes is None else expected_bytes)}


class FetchFileTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.output = Path(self.directory.name) / "font.ttf"
        self.font = b"verified font contents"
        self.checksum = hashlib.sha256(self.font).hexdigest()
        self.sleep = patch.object(fetch_file.time, "sleep")
        self.sleep.start()
        self.addCleanup(self.sleep.stop)

    def fetch(self, archive_member: str = "", cache: Path | None = None):
        arguments = ["fetch_file.py", "--url", "https://example.com/font.zip",
                     "--output", str(self.output), "--sha256", self.checksum]
        if archive_member:
            arguments += ["--archive-member", archive_member]
        if cache is not None:
            arguments += ["--cache", str(cache)]
        with patch.object(fetch_file.sys, "argv", arguments):
            return fetch_file.main()

    def test_interrupted_download_restarts_and_installs_verified_font(self):
        with patch.object(fetch_file.urllib.request, "urlopen", side_effect=[
            Response(self.font[:5], len(self.font)), Response(self.font),
        ]) as network:
            self.assertEqual(self.fetch(), 0)
        self.assertEqual(network.call_count, 2)
        self.assertEqual(self.output.read_bytes(), self.font)
        self.assertEqual(os.listdir(self.output.parent), [self.output.name])

    def test_repeated_interruption_leaves_no_partial_font(self):
        with patch.object(fetch_file.urllib.request, "urlopen", side_effect=[
            Response(b"partial", 100) for _ in range(3)
        ]) as network:
            with self.assertRaises(urllib.error.ContentTooShortError):
                self.fetch()
        self.assertEqual(network.call_count, 3)
        self.assertEqual(os.listdir(self.output.parent), [])

    def test_wrong_archive_member_hash_preserves_existing_output(self):
        self.output.write_bytes(b"existing output")
        archive_bytes = io.BytesIO()
        with zipfile.ZipFile(archive_bytes, "w") as archive:
            archive.writestr("font.ttf", b"wrong font")
        with patch.object(fetch_file.urllib.request, "urlopen",
                          return_value=Response(archive_bytes.getvalue())):
            with self.assertRaisesRegex(RuntimeError, "wrong SHA-256"):
                self.fetch("font.ttf")
        self.assertEqual(self.output.read_bytes(), b"existing output")
        self.assertEqual(os.listdir(self.output.parent), [self.output.name])

    def test_valid_output_can_rebuild_without_network(self):
        self.output.write_bytes(self.font)
        with patch.object(fetch_file.urllib.request, "urlopen") as network:
            self.assertEqual(self.fetch(), 0)
        network.assert_not_called()

    def test_verified_cache_installs_font_without_network(self):
        cache = self.output.parent / "cache"
        cache.mkdir()
        cached_font = cache / self.output.name
        cached_font.write_bytes(self.font)
        with patch.object(fetch_file.urllib.request, "urlopen") as network:
            self.assertEqual(self.fetch(cache=cache), 0)
        network.assert_not_called()
        self.assertEqual(self.output.read_bytes(), self.font)
        self.assertEqual(cached_font.read_bytes(), self.font)
        self.assertCountEqual(os.listdir(self.output.parent), ["cache", self.output.name])

    def test_invalid_cache_preserves_existing_output(self):
        cache = self.output.parent / "cache"
        cache.mkdir()
        (cache / self.output.name).write_bytes(b"invalid cached font")
        self.output.write_bytes(b"existing output")
        with patch.object(fetch_file.urllib.request, "urlopen") as network:
            with self.assertRaisesRegex(RuntimeError, "cached file has the wrong SHA-256"):
                self.fetch(cache=cache)
        network.assert_not_called()
        self.assertEqual(self.output.read_bytes(), b"existing output")
        self.assertCountEqual(os.listdir(self.output.parent), ["cache", self.output.name])


if __name__ == "__main__":
    unittest.main()
