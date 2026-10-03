from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import os
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("ci_gcc", ROOT / "tools" / "ci_gcc.py")
assert spec is not None and spec.loader is not None
ci_gcc = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ci_gcc)


class ToolchainTests(unittest.TestCase):
    def test_both_platforms_select_the_same_release(self):
        linux = ci_gcc.environment("linux")
        windows = ci_gcc.environment("windows")
        self.assertEqual(linux["GCC_VERSION"], windows["GCC_VERSION"])
        self.assertTrue(linux["GCC_PACKAGE_VERSION"].startswith(linux["GCC_VERSION"] + "-"))
        self.assertIn("gcc-" + windows["GCC_VERSION"], windows["GCC_ARCHIVE_URL"])
        self.assertTrue(Path(windows["CC"]).is_absolute())
        self.assertEqual(len(windows["GCC_ARCHIVE_SHA256"]), 64)

    def run_check(self, replies):
        with patch.dict(os.environ, {"CC": "gcc", "CXX": "g++"}), \
             patch.object(ci_gcc.subprocess, "check_output", side_effect=replies), \
             patch.object(ci_gcc.subprocess, "run") as compile_source, \
             contextlib.redirect_stdout(io.StringIO()):
            ci_gcc.check()
            return compile_source

    def test_rejects_old_compiler_before_build(self):
        with self.assertRaisesRegex(RuntimeError, "expected GCC.*got '12.3.0'"):
            self.run_check(["12.3.0\n"])

    def test_rejects_cxx_drift_even_when_c_compiler_matches(self):
        version = json.loads(ci_gcc.PIN.read_text())["gcc_version"]
        with self.assertRaisesRegex(RuntimeError, "CXX: expected GCC"):
            self.run_check([version, "x86_64-linux-gnu", "16.1.0"])

    def test_rejects_wrong_architecture(self):
        version = json.loads(ci_gcc.PIN.read_text())["gcc_version"]
        with self.assertRaisesRegex(RuntimeError, "expected x86_64"):
            self.run_check([version, "aarch64-linux-gnu"])

    def test_matching_compilers_check_actual_clock_cast_source(self):
        version = json.loads(ci_gcc.PIN.read_text())["gcc_version"]
        compile_source = self.run_check([
            version, "x86_64-w64-mingw32", version, "x86_64-w64-mingw32",
        ])
        compile_source.assert_called_once()
        command = compile_source.call_args.args[0]
        self.assertIn("-std=c++20", command)
        self.assertIn("-fsyntax-only", command)
        self.assertEqual(Path(command[-1]), ROOT / "src" / "storage" / "BoardPath.cpp")
        self.assertTrue(compile_source.call_args.kwargs["check"])


if __name__ == "__main__":
    unittest.main()
