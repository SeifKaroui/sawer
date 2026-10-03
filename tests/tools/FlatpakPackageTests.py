"""Verify the Flatpak payload, desktop integration, and sandbox checks."""
from __future__ import annotations

import json
import os
import struct
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE / "tools"))
import package_flatpak
import package_linux
import test_flatpak


def elf_header() -> bytes:
    return b"\x7fELF\x02\x01" + bytes(12) + (62).to_bytes(2, "little")


class FlatpakPackageTests(unittest.TestCase):
    def make_appdir(self, root: Path) -> Path:
        executable = root / "Sawer"
        executable.write_bytes(elf_header() + b"same GCC-built executable")
        library = root / "libstdc++.so.6"
        library.write_bytes(b"compiler runtime fixture")
        appdir = root / "Sawer.AppDir"
        package_linux.stage_appdir(SOURCE, executable, appdir, {library.name: library})
        for name in ("GCC-runtime-copyright.txt", "GNU-GPL-3.txt", "GNU-LGPL-2.1.txt"):
            (appdir / "usr/share/doc/Sawer" / name).write_bytes(b"runtime license fixture")
        return appdir

    def test_payload_preserves_binary_runtimes_and_license_documents(self):
        with tempfile.TemporaryDirectory(prefix="sawer flatpak ") as temporary:
            root = Path(temporary)
            appdir = self.make_appdir(root)
            output = root / "flatpak-source"
            package_flatpak.stage_flatpak(SOURCE, appdir, output, "0.9.0", "2026-10-03")
            payload = output / "payload"
            for original in (appdir / "usr").rglob("*"):
                if original.is_file():
                    copied = payload / original.relative_to(appdir / "usr")
                    self.assertEqual(copied.read_bytes(), original.read_bytes())
            self.assertEqual({path.name for path in (payload / "lib").iterdir()},
                             {"libstdc++.so.6"})
            if os.name == "posix":
                self.assertTrue(os.access(payload / "bin/Sawer", os.X_OK))
            with self.assertRaises(FileExistsError):
                package_flatpak.stage_flatpak(SOURCE, appdir, output, "0.9.0", "2026-10-03")
            self.assertEqual((payload / "bin/Sawer").read_bytes(),
                             (appdir / "usr/bin/Sawer").read_bytes())

    def test_desktop_metadata_file_type_and_icon_match_application_id(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            appdir = self.make_appdir(root)
            output = root / "flatpak-source"
            package_flatpak.stage_flatpak(SOURCE, appdir, output, "0.9.0", "2026-10-03")
            app_id = package_flatpak.APP_ID
            payload = output / "payload"
            manifest = json.loads((output / f"{app_id}.json").read_text())
            self.assertEqual(manifest["app-id"], app_id)
            self.assertEqual(manifest["command"], "Sawer")
            self.assertEqual(manifest["modules"][0]["sources"][0],
                             {"type": "dir", "path": "payload", "dest": "payload"})
            metadata = ET.parse(payload / "share/metainfo" / f"{app_id}.metainfo.xml")
            self.assertEqual(metadata.findtext("id"), app_id)
            self.assertEqual(metadata.findtext("launchable"), app_id + ".desktop")
            self.assertEqual(metadata.find("releases/release").attrib,
                             {"version": "0.9.0", "date": "2026-10-03"})
            desktop = (payload / "share/applications" / f"{app_id}.desktop").read_text()
            self.assertIn("Exec=Sawer %f", desktop)
            self.assertIn("Icon=" + app_id, desktop)
            self.assertIn("MimeType=application/x-sawer;", desktop)
            mime = ET.parse(payload / "share/mime/packages" / f"{app_id}.xml")
            namespace = "{http://www.freedesktop.org/standards/shared-mime-info}"
            self.assertEqual(mime.find(namespace + "mime-type").attrib["type"],
                             "application/x-sawer")
            png = (payload / "share/icons/hicolor/256x256/apps" / f"{app_id}.png").read_bytes()
            self.assertEqual(png[:8], b"\x89PNG\r\n\x1a\n")
            self.assertEqual(struct.unpack_from(">II", png, 16), (256, 256))

    def test_sandbox_limits_filesystem_and_does_not_grant_network_or_full_bus(self):
        manifest = json.loads((SOURCE / "packaging/linux/io.sawer.app.json").read_text())
        permissions = manifest["finish-args"]
        self.assertIn("--socket=wayland", permissions)
        self.assertIn("--socket=fallback-x11", permissions)
        self.assertIn("--device=dri", permissions)
        self.assertEqual([value for value in permissions if value.startswith("--filesystem=")],
                         ["--filesystem=xdg-documents"])
        for forbidden in ("--share=network", "--socket=session-bus", "--socket=system-bus"):
            self.assertNotIn(forbidden, permissions)

    def test_windows_executable_and_invalid_release_values_are_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            appdir = self.make_appdir(root)
            output = root / "flatpak-source"
            for version, release_date in (("0.9", "2026-10-03"),
                                          ("0.9.0", "2026-02-31")):
                with self.assertRaises(ValueError):
                    package_flatpak.stage_flatpak(SOURCE, appdir, output, version, release_date)
                self.assertFalse(output.exists())
            (appdir / "usr/bin/Sawer").write_bytes(b"MZ Windows executable")
            with self.assertRaises(ValueError):
                package_flatpak.stage_flatpak(SOURCE, appdir, output, "0.9.0", "2026-10-03")
            self.assertFalse(output.exists())

    def test_graphics_command_uses_the_selected_backend_and_runtime_driver(self):
        for backend in ("x11", "wayland"):
            command = test_flatpak.render_command(backend)
            self.assertIn("--env=SDL_VIDEODRIVER=" + backend, command)
            self.assertIn("--command=sh", command)
            self.assertIn("/usr/lib/x86_64-linux-gnu/GL/default/share/vulkan/icd.d", command[-1])
            self.assertIn("unset VK_DRIVER_FILES", command[-1])
            self.assertIn("exec /app/bin/Sawer --render-test", command[-1])

    def test_installed_bundle_with_wrong_version_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            appdir = self.make_appdir(root)
            location = root / "installed"
            (location / "files").parent.mkdir(exist_ok=True)
            import shutil
            shutil.copytree(appdir / "usr", location / "files")
            with patch.object(test_flatpak.subprocess, "check_output",
                              side_effect=[str(location), b"Sawer 0.0.0\n"]):
                with self.assertRaisesRegex(ValueError, "unexpected Flatpak version"):
                    test_flatpak.check_metadata(SOURCE, appdir / "usr/bin/Sawer")
            (location / "files/bin/Sawer").write_bytes(b"stale executable")
            with patch.object(test_flatpak.subprocess, "check_output", return_value=str(location)):
                with self.assertRaisesRegex(ValueError, "differs from the release payload"):
                    test_flatpak.check_metadata(SOURCE, appdir / "usr/bin/Sawer")


if __name__ == "__main__":
    unittest.main()
