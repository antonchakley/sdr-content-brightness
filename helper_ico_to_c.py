#!/usr/bin/env python3
"""
ico_to_c.py

Parses a Windows .ico file that contains multiple embedded images (sizes/
color depths) and generates C source (.c) and header (.h) files that embed
each image's raw resource bytes as a static array, plus a helper function

    HICON GetHIconBySize(int size);

that turns the requested size into an HICON at runtime via
CreateIconFromResourceEx(), which is the standard WinAPI call for turning
a single ICONDIRENTRY's raw image bytes into a usable icon handle.

Usage:
    python ico_to_c.py input.ico output_basename

Produces:
    output_basename.h
    output_basename.c

Notes:
  * Each stored image inside an .ico file (a BMP-in-ICO "DIB" chunk, or a
    plain PNG chunk for large icons) is already exactly the payload that
    CreateIconFromResourceEx expects — no ICONDIR wrapper is needed for
    that call, so we just slice the raw bytes straight out of the file.
  * Width/height of 0 in the ICONDIRENTRY means 256 per the ICO spec.
  * If a .ico has more than one image at the same size (e.g. different
    color depths), all are emitted; GetHIconBySize returns the first
    match found for that size, and GetHIconByIndex() is generated too
    for explicit selection.
"""

import struct
import sys
import os


class IconEntry:
    __slots__ = ("width", "height", "color_count", "planes", "bit_count",
                 "bytes_in_res", "image_offset", "data")

    def __init__(self, width, height, color_count, planes, bit_count,
                 bytes_in_res, image_offset):
        self.width = width if width != 0 else 256
        self.height = height if height != 0 else 256
        self.color_count = color_count
        self.planes = planes
        self.bit_count = bit_count
        self.bytes_in_res = bytes_in_res
        self.image_offset = image_offset
        self.data = None  # filled in later


def parse_ico(path):
    with open(path, "rb") as f:
        raw = f.read()

    if len(raw) < 6:
        raise ValueError("File too small to be a valid .ico")

    reserved, res_type, count = struct.unpack_from("<HHH", raw, 0)
    if reserved != 0 or res_type != 1:
        raise ValueError("Not a valid .ico file (bad ICONDIR header)")
    if count == 0:
        raise ValueError(".ico file contains no images")

    entries = []
    offset = 6
    for i in range(count):
        (bWidth, bHeight, bColorCount, bReserved,
         wPlanes, wBitCount, dwBytesInRes, dwImageOffset) = struct.unpack_from(
            "<BBBBHHII", raw, offset)
        entries.append(IconEntry(bWidth, bHeight, bColorCount, wPlanes,
                                  wBitCount, dwBytesInRes, dwImageOffset))
        offset += 16

    for e in entries:
        start = e.image_offset
        end = start + e.bytes_in_res
        if end > len(raw):
            raise ValueError("Corrupt .ico: image data runs past end of file")
        e.data = raw[start:end]

    return entries


def bytes_to_c_array(data, indent="    "):
    lines = []
    line = []
    for i, b in enumerate(data):
        line.append("0x%02X" % b)
        if len(line) == 16:
            lines.append(indent + ", ".join(line) + ",")
            line = []
    if line:
        lines.append(indent + ", ".join(line) + ",")
    return "\n".join(lines)


def sanitize_ident(s):
    return "".join(c if c.isalnum() else "_" for c in s)


def generate(entries, basename, out_dir):
    header_name = basename + ".h"
    source_name = basename + ".c"
    guard = sanitize_ident(basename).upper() + "_H"

    # ---- header ----
    header_lines = []
    header_lines.append("#ifndef %s" % guard)
    header_lines.append("#define %s" % guard)
    header_lines.append("")
    header_lines.append("#include <windows.h>")
    header_lines.append("")
    header_lines.append("/* Returns an HICON for the first embedded image matching")
    header_lines.append(" * the requested size (width == height == size), or NULL")
    header_lines.append(" * if no image of that size is embedded.")
    header_lines.append(" * Caller owns the returned handle and should call")
    header_lines.append(" * DestroyIcon() on it when done. */")
    header_lines.append("HICON GetHIconBySize(int size);")
    header_lines.append("")
    header_lines.append("/* Returns an HICON for the Nth embedded image (0-based),")
    header_lines.append(" * regardless of size. Caller owns the returned handle. */")
    header_lines.append("HICON GetHIconByIndex(int index);")
    header_lines.append("")
    header_lines.append("/* Number of images embedded in this file. */")
    header_lines.append("extern const int g_IconImageCount;")
    header_lines.append("")
    header_lines.append("#endif /* %s */" % guard)
    header_lines.append("")

    # ---- source ----
    src_lines = []
    src_lines.append('#include "%s"' % header_name)
    src_lines.append("")

    for idx, e in enumerate(entries):
        arr_name = "g_IconData_%d_%dx%d" % (idx, e.width, e.height)
        src_lines.append("/* Image %d: %dx%d, %d-bit, %d bytes */" %
                          (idx, e.width, e.height, e.bit_count, len(e.data)))
        src_lines.append("static const unsigned char %s[] = {" % arr_name)
        src_lines.append(bytes_to_c_array(e.data))
        src_lines.append("};")
        src_lines.append("")

    src_lines.append("typedef struct {")
    src_lines.append("    int width;")
    src_lines.append("    int height;")
    src_lines.append("    const unsigned char *data;")
    src_lines.append("    unsigned int size;")
    src_lines.append("} IconResource;")
    src_lines.append("")

    src_lines.append("static const IconResource g_IconResources[] = {")
    for idx, e in enumerate(entries):
        arr_name = "g_IconData_%d_%dx%d" % (idx, e.width, e.height)
        src_lines.append("    { %d, %d, %s, sizeof(%s) }," %
                          (e.width, e.height, arr_name, arr_name))
    src_lines.append("};")
    src_lines.append("")
    src_lines.append("const int g_IconImageCount = %d;" % len(entries))
    src_lines.append("")

    src_lines.append("HICON GetHIconByIndex(int index)")
    src_lines.append("{")
    src_lines.append("    if (index < 0 || index >= g_IconImageCount)")
    src_lines.append("        return NULL;")
    src_lines.append("")
    src_lines.append("    const IconResource *res = &g_IconResources[index];")
    src_lines.append("    return CreateIconFromResourceEx(")
    src_lines.append("        (PBYTE)res->data,")
    src_lines.append("        res->size,")
    src_lines.append("        TRUE,           /* fIcon: TRUE = icon, FALSE = cursor */")
    src_lines.append("        0x00030000,     /* resource version, per MSDN */")
    src_lines.append("        res->width,")
    src_lines.append("        res->height,")
    src_lines.append("        LR_DEFAULTCOLOR);")
    src_lines.append("}")
    src_lines.append("")

    src_lines.append("HICON GetHIconBySize(int size)")
    src_lines.append("{")
    src_lines.append("    for (int i = 0; i < g_IconImageCount; i++) {")
    src_lines.append("        if (g_IconResources[i].width == size &&")
    src_lines.append("            g_IconResources[i].height == size) {")
    src_lines.append("            return GetHIconByIndex(i);")
    src_lines.append("        }")
    src_lines.append("    }")
    src_lines.append("    return NULL;")
    src_lines.append("}")
    src_lines.append("")

    header_path = os.path.join(out_dir, header_name)
    source_path = os.path.join(out_dir, source_name)

    with open(header_path, "w", newline="\n") as f:
        f.write("\n".join(header_lines))
    with open(source_path, "w", newline="\n") as f:
        f.write("\n".join(src_lines))

    return header_path, source_path


def main():
    if len(sys.argv) != 3:
        print("Usage: python ico_to_c.py <input.ico> <output_basename>")
        sys.exit(1)

    ico_path = sys.argv[1]
    basename = sys.argv[2]
    out_dir = os.path.dirname(os.path.abspath(basename)) or "."
    basename = os.path.basename(basename)

    entries = parse_ico(ico_path)

    print("Found %d embedded image(s):" % len(entries))
    for i, e in enumerate(entries):
        print("  [%d] %dx%d, %d-bit, %d bytes" %
              (i, e.width, e.height, e.bit_count, len(e.data)))

    header_path, source_path = generate(entries, basename, out_dir)
    print("Wrote:")
    print("  %s" % header_path)
    print("  %s" % source_path)


if __name__ == "__main__":
    main()