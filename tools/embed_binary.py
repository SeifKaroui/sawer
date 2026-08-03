from __future__ import annotations

import argparse
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("symbol")
    arguments = parser.parse_args()

    data = Path(arguments.input).read_bytes()
    rows = []
    for offset in range(0, len(data), 16):
        rows.append(
            "    "
            + ", ".join(f"0x{value:02x}" for value in data[offset : offset + 16])
            + ","
        )
    source = (
        "#include <cstddef>\n"
        "namespace sawer::assets {\n"
        f"extern const unsigned char {arguments.symbol}[] = {{\n"
        + "\n".join(rows)
        + "\n};\n"
        f"extern const std::size_t {arguments.symbol}_size = "
        f"sizeof({arguments.symbol});\n"
        "}\n"
    )
    Path(arguments.output).write_text(source, encoding="utf-8", newline="\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
