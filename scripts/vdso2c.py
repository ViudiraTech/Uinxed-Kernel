#!/usr/bin/env python3
"""Turn the linked vDSO ELF into a C array the kernel embeds.

Usage: vdso2c.py <vdso.so> <symbol-prefix> > vdso_image.c
"""
import sys

def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2

    path, prefix = sys.argv[1], sys.argv[2]
    with open(path, "rb") as handle:
        blob = handle.read()

    out = sys.stdout
    out.write("/*\n *\n *      %s.c\n" % prefix)
    out.write(" *      Generated from %s - do not edit\n *\n" % path)
    out.write(" *      2026/10/4 By JiTianYu391\n")
    out.write(" *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.\n *\n */\n\n")
    out.write("#include <libs/std/stdint.h>\n\n")
    out.write("const uint8_t %s[] = {\n" % prefix)
    for offset in range(0, len(blob), 16):
        chunk = blob[offset:offset + 16]
        out.write("    " + " ".join("0x%02x," % byte for byte in chunk) + "\n")
    out.write("};\n\n")
    out.write("const uint32_t %s_len = %uu;\n" % (prefix, len(blob)))
    return 0

if __name__ == "__main__":
    sys.exit(main())
