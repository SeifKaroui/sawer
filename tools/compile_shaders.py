"""Regenerate embedded DXIL and SPIR-V with an offline DXC executable."""
from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--dxc", required=True)
    parser.add_argument("--output-dir", default="build/shader-bytecode")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    output = root / args.output_dir
    output.mkdir(parents=True, exist_ok=True)
    for name in ("background", "retained"):
        for stage, profile in (("vert", "vs_6_0"), ("frag", "ps_6_0")):
            source = root / "assets" / "shaders" / f"{name}.{stage}.hlsl"
            for format_name in ("dxil", "spv"):
                binary = output / f"{name}.{stage}.{format_name}"
                command = [args.dxc, "-T", profile, "-E", "main", "-O3", "-WX",
                           "-Fo", str(binary), str(source)]
                if format_name == "spv":
                    command += ["-spirv", "-fspv-target-env=vulkan1.0"]
                subprocess.run(command, check=True)
                data = binary.read_bytes()
                symbol = f"{name}_{stage}_{format_name}"
                rows = ["    " + ", ".join(f"0x{byte:02x}" for byte in data[i:i+16]) + ","
                        for i in range(0, len(data), 16)]
                header = "#pragma once\n#include <cstddef>\n"
                header += f"inline constexpr unsigned char {symbol}[] = {{\n"
                header += "\n".join(rows) + "\n};\n"
                header += f"inline constexpr std::size_t {symbol}_len = sizeof({symbol});\n"
                (root / "assets" / "shaders" / "generated" / f"{name}.{stage}.{format_name}.h").write_text(
                    header, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
