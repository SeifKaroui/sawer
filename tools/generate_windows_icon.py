from __future__ import annotations

import argparse
import io
import struct
import sys
from pathlib import Path

from PIL import Image


ICON_SIZES = (16, 24, 32, 48, 64, 128, 256)


def render_icon(source: Path) -> bytes:
    with Image.open(source) as opened:
        image = opened.convert("RGBA")
    if image.width != image.height:
        raise ValueError(
            f"Windows icon source must be square, got {image.width}x{image.height}"
        )

    output = io.BytesIO()
    image.save(output, format="ICO", sizes=[(size, size) for size in ICON_SIZES])
    data = output.getvalue()
    reserved, kind, count = struct.unpack_from("<HHH", data)
    if reserved != 0 or kind != 1 or count != len(ICON_SIZES):
        raise ValueError("Generated ICO does not contain the expected image set")
    return data


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "--check",
        action="store_true",
        help="fail if output differs instead of writing it",
    )
    arguments = parser.parse_args()

    generated = render_icon(arguments.source)
    if arguments.check:
        if not arguments.output.exists():
            print(f"missing generated icon: {arguments.output}", file=sys.stderr)
            return 1
        if arguments.output.read_bytes() != generated:
            print(
                f"{arguments.output} is stale; regenerate it from {arguments.source}",
                file=sys.stderr,
            )
            return 1
        return 0

    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_bytes(generated)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
