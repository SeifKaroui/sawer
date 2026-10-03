"""Exercise the real command-line parser with quoted, non-ANSI file paths."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> None:
    executable = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="sawer-unicode-") as temporary:
        directory = Path(temporary) / "\u753b\u677f folder \U0001f58a"
        directory.mkdir()
        board = directory / "\u8ba1\u5212 'quoted' board.sawer"
        header = {"format": "sawer", "version": 1,
                  "board_id": "00000000-0000-0000-0000-000000000001",
                  "bounds": [-1000000, -1000000, 1000000, 1000000]}
        board.write_text(json.dumps(header) + "\n", encoding="utf-8")
        for command in ("validate", "compact", "info", "dump", "validate"):
            subprocess.run([executable, command, str(board)], check=True,
                           capture_output=True, timeout=15)
        if not board.read_bytes().startswith(b"SAWER\0\x01\0"):
            raise AssertionError("compact did not write the Unicode-named board")
        extraction = directory / "\u56fe\u7247 output"
        subprocess.run([executable, "extract", str(board), str(extraction)],
                       check=True, capture_output=True, timeout=15)
        json.loads((extraction / "manifest.json").read_text(encoding="utf-8"))
        missing = directory / "\u4e0d\u5b58\u5728.sawer"
        rejected = subprocess.run([executable, "validate", str(missing)],
                                  capture_output=True, timeout=15)
        if rejected.returncode == 0:
            raise AssertionError("a missing Unicode-named board was accepted")


if __name__ == "__main__":
    main()
