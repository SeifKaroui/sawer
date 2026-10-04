"""Extract ZIP/tar packages and resolve the documentation's local links."""
from __future__ import annotations

import re
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path
from urllib.parse import unquote, urlsplit


SOURCE = Path(__file__).resolve().parents[2]
DOCUMENTS = [*(SOURCE / "docs" / name for name in (
                 "file-format.md", "releasing.md", "board-loading-performance.md",
                 "rendering-performance.md")),
             SOURCE / "assets/shaders/README.md", SOURCE / "tools/compile_shaders.py"]


class PortablePackageTests(unittest.TestCase):
    def test_extracted_documentation_links_and_executable(self) -> None:
        for platform, extension in (("windows", ".zip"), ("linux", ".tar.gz")):
            with self.subTest(extension=extension), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                executable = directory / "Sawer.exe"
                executable.write_bytes(b"standalone application fixture")
                package_name = f"sawer-v0.9.0-{platform}-x64"
                archive = directory / (package_name + extension)
                subprocess.run([
                    sys.executable, str(SOURCE / "tools/package_portable.py"),
                    "--executable", str(executable), "--readme", str(SOURCE / "README.md"),
                    "--banner", str(SOURCE / "assets/banner.png"),
                    "--notices", str(SOURCE / "THIRD_PARTY_NOTICES.md"),
                    "--source-root", str(SOURCE), "--output", str(archive), "--root", package_name,
                    "--documentation", *map(str, DOCUMENTS),
                ], check=True, timeout=30)
                extraction = directory / "extracted"
                if extension == ".zip":
                    with zipfile.ZipFile(archive) as packed:
                        packed.extractall(extraction)
                else:
                    with tarfile.open(archive) as packed:
                        options = {"filter": "data"} if hasattr(tarfile, "data_filter") else {}
                        packed.extractall(extraction, **options)
                root = extraction / package_name
                self.assertEqual((root / executable.name).read_bytes(), executable.read_bytes())
                self.assertEqual((root / "THIRD_PARTY_NOTICES.md").read_bytes(),
                                 (SOURCE / "THIRD_PARTY_NOTICES.md").read_bytes())
                for document in root.rglob("*.md"):
                    body = document.read_text(encoding="utf-8")
                    targets = re.findall(r'!?\[[^\]]*\]\(([^)]+)\)', body)
                    targets += re.findall(r'src="([^"]+)"', body)
                    for target in targets:
                        if not urlsplit(target).scheme and not target.startswith("#"):
                            self.assertTrue((document.parent / unquote(target.split("#")[0])).exists(),
                                            f"{document.relative_to(root)}: {target}")


if __name__ == "__main__":
    unittest.main()
