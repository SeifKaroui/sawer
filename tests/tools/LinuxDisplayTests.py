"""Verify display-command deadlines, retained diagnostics, and child cleanup."""
from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

SOURCE = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SOURCE / "tools"))
import test_linux_display


class LinuxDisplayTests(unittest.TestCase):
    def test_command_retains_both_streams_and_failure_status(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "command.log"
            command = [sys.executable, "-c",
                       "import sys; print('frame check'); print('GPU error', file=sys.stderr); sys.exit(7)"]
            result = test_linux_display.run_logged_command(command, os.environ.copy(), log, 10)
            self.assertEqual(result, 7)
            output = log.read_text(encoding="utf-8")
            self.assertIn("frame check", output)
            self.assertIn("GPU error", output)

    def test_timeout_is_prompt_and_keeps_partial_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "command.log"
            command = [sys.executable, "-c",
                       "import time; print('starting frame check', flush=True); time.sleep(60)"]
            started = time.monotonic()
            with self.assertRaisesRegex(TimeoutError, "command exceeded 1s"):
                test_linux_display.run_logged_command(command, os.environ.copy(), log, 1)
            self.assertLess(time.monotonic() - started, 10)
            self.assertIn("starting frame check", log.read_text(encoding="utf-8"))
            if sys.platform == "linux":
                self.assertIn("PID", log.with_suffix(".timeout.txt").read_text(encoding="utf-8"))

    def test_nonpositive_timeout_does_not_launch_a_process(self):
        with patch.object(test_linux_display.subprocess, "Popen") as popen:
            with self.assertRaises(ValueError):
                test_linux_display.run_logged_command(["unused"], {}, Path("unused.log"), 0)
            popen.assert_not_called()

    def test_preferences_preserve_only_application_logs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "preferences"
            folder = root / "data/Sawer"
            folder.mkdir(parents=True)
            (folder / "Sawer.log").write_bytes(b"partial GPU log")
            (folder / "Sawer.log.previous").write_bytes(b"previous log")
            (folder / "recent-files.json").write_bytes(b"private preferences")
            destination = Path(temporary) / "saved"
            test_linux_display.preserve_application_logs(root, destination)
            self.assertEqual((destination / "data/Sawer/Sawer.log").read_bytes(), b"partial GPU log")
            self.assertEqual((destination / "data/Sawer/Sawer.log.previous").read_bytes(), b"previous log")
            self.assertEqual(len(list(destination.rglob("*.*"))), 2)

    def test_session_preserves_logs_after_command_failure_and_selects_host_driver(self):
        with tempfile.TemporaryDirectory() as temporary:
            log_dir = Path(temporary) / "saved"
            def fail(command, environment, log_path, timeout):
                self.assertEqual(timeout, 120)
                self.assertNotIn("VK_DRIVER_FILES", environment)
                self.assertNotIn("VK_ADD_DRIVER_FILES", environment)
                self.assertEqual(environment["VK_ICD_FILENAMES"], str(Path("/fake/lvp_icd.json")))
                folder = Path(environment["XDG_DATA_HOME"]) / "Sawer"
                folder.mkdir(parents=True)
                (folder / "Sawer.log").write_bytes(b"startup failed")
                raise TimeoutError("test deadline")
            original_glob = Path.glob
            def driver_glob(path, pattern, *args, **kwargs):
                if pattern == "lvp_icd*.json":
                    return iter([Path("/fake/lvp_icd.json")])
                return original_glob(path, pattern, *args, **kwargs)
            with patch.dict(os.environ, {"VK_DRIVER_FILES": "other.json", "VK_ADD_DRIVER_FILES": "other.json"}), \
                    patch.object(test_linux_display.Path, "glob", autospec=True, side_effect=driver_glob), \
                    patch.object(test_linux_display, "wait_until_ready"), \
                    patch.object(test_linux_display.subprocess, "Popen") as popen, \
                    patch.object(test_linux_display, "stop_process") as stop, \
                    patch.object(test_linux_display, "run_logged_command", side_effect=fail):
                with self.assertRaisesRegex(TimeoutError, "test deadline"):
                    test_linux_display.run_session("wayland", ["Sawer", "--render-test"], log_dir)
                stop.assert_called_once_with(popen.return_value)
            self.assertEqual((log_dir / "wayland-preferences/data/Sawer/Sawer.log").read_bytes(),
                             b"startup failed")

    @unittest.skipUnless(sys.platform == "linux", "Linux process-group integration")
    def test_launcher_children_are_stopped_on_success_and_timeout(self):
        # An uncooperative child survives terminating just the launcher.
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for timed_out in (False, True):
                with self.subTest(timed_out=timed_out):
                    marker = root / f"child-{timed_out}.pid"
                    child = ("import os, signal, time; from pathlib import Path; "
                             "signal.signal(signal.SIGTERM, signal.SIG_IGN); "
                             f"Path({str(marker)!r}).write_text(str(os.getpid())); time.sleep(60)")
                    parent = ("import subprocess, sys, time; from pathlib import Path; "
                              f"subprocess.Popen([sys.executable, '-c', {child!r}]); "
                              f"marker = Path({str(marker)!r}); "
                              "deadline = time.monotonic() + 5; "
                              "\nwhile not marker.exists() and time.monotonic() < deadline: time.sleep(.01)\n"
                              "assert marker.exists(); "
                              + ("time.sleep(60)" if timed_out else "sys.exit(0)"))
                    command = [sys.executable, "-c", parent]
                    try:
                        if timed_out:
                            with self.assertRaises(TimeoutError):
                                test_linux_display.run_logged_command(command, os.environ.copy(),
                                                                      root / "timeout.log", 1)
                        else:
                            self.assertEqual(test_linux_display.run_logged_command(
                                command, os.environ.copy(), root / "success.log", 10), 0)
                        child_pid = int(marker.read_text())
                        deadline = time.monotonic() + 3
                        while time.monotonic() < deadline:
                            status = Path(f"/proc/{child_pid}/status")
                            try:
                                if "State:\tZ" in status.read_text():
                                    break
                            except FileNotFoundError:
                                break
                            time.sleep(.01)
                        else:
                            self.fail("launcher child remains running after cleanup")
                    finally:
                        if marker.exists():
                            try:
                                os.kill(int(marker.read_text()), signal.SIGKILL)
                            except ProcessLookupError:
                                pass


if __name__ == "__main__":
    unittest.main()
