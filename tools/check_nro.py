#!/usr/bin/env python3
"""Reads back what an NRO says about itself: its name, its author and its icon.

    python tools/check_nro.py file.nro "Expected name" hazevauks
"""

import struct
import sys


def main():
    path, name, author = sys.argv[1:4]
    with open(path, "rb") as nro:
        data = nro.read()

    if data[0x10:0x14] != b"NRO0":
        sys.exit("%s is not an NRO" % path)
    size = struct.unpack_from("<I", data, 0x18)[0]
    if data[size:size + 4] != b"ASET":
        sys.exit("%s has no assets" % path)
    icon_offset, icon_size, nacp_offset, nacp_size = struct.unpack_from("<QQQQ", data, size + 8)

    nacp = data[size + nacp_offset:size + nacp_offset + nacp_size]
    found_name = nacp[0:0x200].split(b"\0")[0].decode()
    found_author = nacp[0x200:0x300].split(b"\0")[0].decode()

    icon = data[size + icon_offset:size + icon_offset + icon_size]
    if icon[:2] != b"\xff\xd8":
        sys.exit("the icon is not a JPEG")
    width = height = None
    at = 2
    while at < len(icon):
        marker = icon[at + 1]
        length = struct.unpack_from(">H", icon, at + 2)[0]
        if marker in (0xC0, 0xC1, 0xC2):
            height, width = struct.unpack_from(">HH", icon, at + 5)
            break
        at += 2 + length

    print("%s: %r by %r, icon %sx%s JPEG, %.1f MB" % (path, found_name, found_author, width, height, len(data) / 1048576))
    if found_name != name or found_author != author or (width, height) != (256, 256):
        sys.exit("expected %r by %r with a 256x256 icon" % (name, author))


if __name__ == "__main__":
    main()
