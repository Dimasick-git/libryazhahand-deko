#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Шейдеры стекла (libglass/shaders) -> libglass/source/glass_shaders.cpp.

Оверлеи собирают стекло как обычные исходники, без uam и правил сборки шейдеров: код шейдеров лежит в
glass_shaders.cpp готовыми массивами. После правки шейдера запустить этот скрипт (нужен uam из devkitPro:
$DEVKITPRO/tools/bin или PATH) и закоммитить glass_shaders.cpp вместе с шейдером.
"""
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHADERS = [
    ("present.vert", "vert", "glass_present_vert_dksh"),
    ("present.frag", "frag", "glass_present_frag_dksh"),
    ("capture.frag", "frag", "glass_capture_frag_dksh"),
    ("blur_down.frag", "frag", "glass_blur_down_frag_dksh"),
    ("blur_up.frag", "frag", "glass_blur_up_frag_dksh"),
]


def find_uam():
    for base in filter(None, [os.environ.get("DEVKITPRO"), "/opt/devkitpro", "C:/devkitPro"]):
        for name in ("uam", "uam.exe"):
            path = os.path.join(base, "tools", "bin", name)
            if os.path.isfile(path):
                return path
    return shutil.which("uam")


def main():
    uam = find_uam()
    if not uam:
        sys.exit("uam not found: set DEVKITPRO or put uam on PATH")
    out = []
    out.append("// SPDX-License-Identifier: GPL-2.0-only")
    out.append("// Сгенерировано libglass/tools/build_shaders.py из libglass/shaders — руками не править.")
    out.append('#include "glass_shaders.hpp"')
    out.append("")
    with tempfile.TemporaryDirectory() as tmp:
        for name, stage, symbol in SHADERS:
            dksh = os.path.join(tmp, name + ".dksh")
            subprocess.run([uam, "-s", stage, "-o", dksh, os.path.join(ROOT, "shaders", name)], check=True)
            data = open(dksh, "rb").read()
            out.append(f"// shaders/{name}")
            out.append(f"alignas(256) const u8 {symbol}[] = {{")
            for i in range(0, len(data), 16):
                out.append("    " + ", ".join(f"0x{b:02x}" for b in data[i:i + 16]) + ",")
            out.append("};")
            out.append(f"const u32 {symbol}_size = sizeof({symbol});")
            out.append("")
    with open(os.path.join(ROOT, "source", "glass_shaders.cpp"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("glass_shaders.cpp: %d shaders" % len(SHADERS))


if __name__ == "__main__":
    main()
