"""Check Linux payload validation, dependency isolation, and launcher behavior."""
from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from urllib.parse import unquote, urlsplit

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE / "tools"))
import package_linux


def elf_header(machine: int = 62, bits: int = 2) -> bytes:
    return b"\x7fELF" + bytes([bits, 1]) + bytes(12) + machine.to_bytes(2, "little")


class LinuxPackageTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which("bash"), "Bash is required for workflow checksum checks")
    def test_versioned_download_checksums_and_tamper_detection(self) -> None:
        workflow = (SOURCE / ".github/workflows/linux-release.yml").read_text(encoding="utf-8")
        step = re.search(
            r"(?ms)^      - name: Create Linux checksums\n.*?        run: \|\n"
            r"(?P<body>.*?)(?=^      - name:)", workflow)
        self.assertIsNotNone(step)
        body = re.sub(r"(?m)^          ", "", step.group("body"))
        stem = "sawer-v0.9.0-linux-x64"
        payload = {stem + suffix: ("fixture " + suffix).encode() for suffix in (
            "", ".AppImage", ".flatpak", "-third-party-notices.md",
            "-dependencies.txt", "-flatpak-runtime.txt")}
        with tempfile.TemporaryDirectory(prefix="sawer release downloads ") as temporary:
            root = Path(temporary)
            distribution = root / "build/linux-dist"
            distribution.mkdir(parents=True)
            for name, contents in payload.items():
                (distribution / name).write_bytes(contents)
            environment = dict(os.environ, RELEASE_STEM=stem)
            subprocess.run([shutil.which("bash"), "-e", "-c", body], cwd=root,
                           env=environment, check=True, capture_output=True, text=True)
            manifest = distribution / (stem + "-sha256sums.txt")
            # GNU coreutils on Windows marks binary mode with '*'; Linux uses a space.
            entries = set()
            for line in manifest.read_text().splitlines():
                self.assertRegex(line, r"^[0-9a-f]{64} [ *]")
                entries.add((line[:64], line[66:]))
            self.assertEqual(entries, {
                (hashlib.sha256(contents).hexdigest(), name)
                for name, contents in payload.items()})
            (distribution / stem).write_bytes(b"modified download")
            checked = subprocess.run([
                shutil.which("bash"), "-e", "-c",
                'cd build/linux-dist; sha256sum --check "$RELEASE_STEM-sha256sums.txt"'],
                cwd=root, env=environment, capture_output=True, text=True)
            self.assertNotEqual(checked.returncode, 0)
            (distribution / (stem + "-dependencies.txt")).unlink()
            incomplete = subprocess.run([shutil.which("bash"), "-e", "-c", body],
                                        cwd=root, env=environment, capture_output=True, text=True)
            self.assertNotEqual(incomplete.returncode, 0)

    def test_only_x86_64_elf_is_accepted(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / "Sawer"
            executable.write_bytes(elf_header())
            package_linux.check_executable(executable)
            for payload in (b"MZ Windows executable", b"\x7fELF", elf_header(183),
                            elf_header(bits=1)):
                with self.subTest(payload=payload):
                    executable.write_bytes(payload)
                    with self.assertRaises(ValueError):
                        package_linux.check_executable(executable)

    def test_dependency_paths_with_spaces_and_host_loader(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sawer library ") as temporary:
            root = Path(temporary)
            libc = root / "libc.so.6"
            runtime = root / "libstdc++.so.6"
            for path in (libc, runtime):
                path.write_bytes(b"library")
            output = (f"linux-vdso.so.1 (0x1234)\n"
                      f"libstdc++.so.6 => {runtime.as_posix()} (0xabcd)\n"
                      f"libc.so.6 => {libc.as_posix()} (0x1234)\n")
            libraries = package_linux.parse_dependencies(output)
            self.assertEqual(libraries, {"libstdc++.so.6": runtime, "libc.so.6": libc})
            if sys.platform == "linux":
                loader = root / "ld-linux-x86-64.so.2"
                loader.write_bytes(b"loader")
                libraries = package_linux.parse_dependencies(
                    output + f"{loader} (0x4567)\n")
                self.assertEqual(libraries["ld-linux-x86-64.so.2"], loader)

    def test_missing_unknown_and_malformed_dependencies_fail(self) -> None:
        for output in ("libstdc++.so.6 => not found\n",
                       "libSDL3.so.0 => /lib/libSDL3.so.0 (0x1234)\n",
                       "libvulkan.so.1 => /lib/libvulkan.so.1 (0x1234)\n",
                       "libc.so.6 => relative/path (0x1234)\n",
                       "libc.so.6 => /missing-sawer-fixture/libc.so.6 (0x1234)\n",
                       "statically linked\n", "", "garbage\n"):
            with self.subTest(output=output):
                with self.assertRaises(ValueError):
                    package_linux.parse_dependencies(output)

    def test_duplicate_dependencies_fail(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            library = Path(temporary) / "libc.so.6"
            library.touch()
            line = f"libc.so.6 => {library.as_posix()} (0x1234)\n"
            with self.assertRaisesRegex(ValueError, "duplicate"):
                package_linux.parse_dependencies(line * 2)

    def test_appdir_contains_identical_executable_and_only_compiler_libraries(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sawer package ") as temporary:
            root = Path(temporary)
            executable = root / "Sawer"
            executable.write_bytes(elf_header() + b"executable fixture")
            libraries = {}
            for name in ("libc.so.6", "libstdc++.so.6", "libgcc_s.so.1"):
                library = root / name
                library.write_bytes(name.encode())
                libraries[name] = library
            appdir = root / "Sawer.AppDir"
            package_linux.stage_appdir(SOURCE, executable, appdir, libraries)
            self.assertEqual((appdir / "usr/bin/Sawer").read_bytes(), executable.read_bytes())
            self.assertEqual({path.name for path in (appdir / "usr/lib").iterdir()},
                             {"libstdc++.so.6", "libgcc_s.so.1"})
            for name in ("libstdc++.so.6", "libgcc_s.so.1"):
                self.assertEqual((appdir / "usr/lib" / name).read_bytes(),
                                 libraries[name].read_bytes())
            self.assertEqual((appdir / "usr/share/doc/Sawer/THIRD_PARTY_NOTICES.md").read_bytes(),
                             (SOURCE / "THIRD_PARTY_NOTICES.md").read_bytes())
            self.assertTrue((appdir / "usr/share/doc/Sawer/AppImage-runtime-LICENSE.txt").is_file())
            documentation = appdir / "usr/share/doc/Sawer"
            for document in documentation.rglob("*.md"):
                body = document.read_text(encoding="utf-8")
                targets = re.findall(r'!?\[[^\]]*\]\(([^)]+)\)', body)
                targets += re.findall(r'src="([^"]+)"', body)
                for target in targets:
                    if not urlsplit(target).scheme and not target.startswith("#"):
                        self.assertTrue((document.parent / unquote(target.split("#")[0])).exists(),
                                        f"{document.relative_to(documentation)}: {target}")
            desktop = (appdir / "io.sawer.app.desktop").read_text()
            self.assertIn("Exec=Sawer %f", desktop)
            self.assertIn("Icon=io.sawer.app", desktop)
            self.assertTrue((appdir / "io.sawer.app.png").is_file())
            self.assertNotIn(b"\r", (appdir / "AppRun").read_bytes())
            if os.name == "posix":
                self.assertTrue(os.access(appdir / "AppRun", os.X_OK))
                self.assertTrue(os.access(appdir / "usr/bin/Sawer", os.X_OK))
            with self.assertRaises(FileExistsError):
                package_linux.stage_appdir(SOURCE, executable, appdir, libraries)
            self.assertEqual((appdir / "usr/bin/Sawer").read_bytes(), executable.read_bytes())

    @unittest.skipUnless(sys.platform == "linux", "AppRun requires a Linux shell")
    def test_launcher_preserves_working_directory_and_unicode_arguments(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sawer launcher ") as temporary:
            root = Path(temporary)
            executable = root / "fixture"
            executable.write_text('#!/bin/sh\nprintf "%s\\n" "$PWD" "$@" "$LD_LIBRARY_PATH"\n')
            appdir = root / "Sawer.AppDir"
            package_linux.stage_appdir(SOURCE, executable, appdir, {})
            working = root / "different folder"
            working.mkdir()
            board = "画板 folder/plan 'quoted' 🖊.sawer"
            environment = os.environ.copy()
            environment.pop("APPDIR", None)
            environment["LD_LIBRARY_PATH"] = "/existing/library/path"
            result = subprocess.run([str(appdir / "AppRun"), board], cwd=working,
                                    env=environment, check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout.splitlines(),
                             [str(working), board,
                              f"{appdir}/usr/lib:/existing/library/path"])


if __name__ == "__main__":
    unittest.main()
