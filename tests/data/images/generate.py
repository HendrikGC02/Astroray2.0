# SPDX-License-Identifier: Apache-2.0
"""Generates the tiny 4x3 test images beside this file (task 060). Run through
tools/lib/blender.ps1 Invoke-ArBlender (the JPEG goes through Blender's own encoder); the
PNG and TGA files are written byte by byte so they are exact and reproducible.
The pattern is asymmetric on purpose: swapping rows or channels changes every texel.
Row y = 0 is the BOTTOM row, Blender's order; the files store it the way each format does.
"""
import os
import struct
import zlib

W, H = 4, 3
OUT = os.path.dirname(os.path.abspath(__file__))


def texel(x, y, c):
    # Shared with tests/cpu/test_image_ingest.cpp (pattern()).
    return ((x + 1) * 47 + (y + 1) * 83 + c * 61) % 256


def chunk(tag, data):
    body = tag + data
    return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)


def png_bytes(channels):
    color_type = {1: 0, 3: 2, 4: 6}[channels]
    raw = b""
    for top in range(H):  # PNG stores the top row first
        y = H - 1 - top
        raw += b"\x00" + bytes(texel(x, y, c) for x in range(W) for c in range(channels))
    ihdr = struct.pack(">IIBBBBB", W, H, 8, color_type, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def tga_bytes():
    # Uncompressed 24-bit, bottom-left origin (descriptor bit 5 clear): rows are stored bottom first, BGR.
    head = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, W, H, 24, 0)
    body = b"".join(bytes(texel(x, y, c) for c in (2, 1, 0)) for y in range(H) for x in range(W))
    return head + body


def write(name, data):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(data)


png3 = png_bytes(3)
write("pattern_4x3_rgb.png", png3)
write("pattern_4x3_gray.png", png_bytes(1))
write("pattern_4x3_rgba.png", png_bytes(4))
write("pattern_4x3.tga", tga_bytes())
write("truncated.png", png3[: len(png3) * 2 // 3])  # cut inside IDAT

# ---- HDR images: OpenEXR and Radiance, written byte by byte -------------------------------------
# Rows are stored top first by both formats; texel (x, y) has y = 0 at the bottom, as above.
# The values are shared with tests/cpu/test_image_ingest.cpp (half_bits(), float_value()).
# Half: 0, 1e-5 (a half subnormal), the smallest subnormal, the smallest normal, 0.25, 0.5, 1,
# 2.5, -0.375, 995.0 (0x63D0), 65504.
HALF_BITS = [0x0000, 0x00A8, 0x0001, 0x0400, 0x3400, 0x3800, 0x3C00, 0x4100, 0xB600, 0x63D0, 0x7BFF]
FLOAT_VALUES = [0.0, 1e-5, 0.25, 1.0, 65504.0, 1e5, -0.75, 3.25, 123456.789]


def hdr_index(x, y, c, n):
    return ((x + 1) * 47 + (y + 1) * 83 + c * 61) % n


def half_bits(x, y, c):
    return HALF_BITS[hdr_index(x, y, c, len(HALF_BITS))]


def float_value(x, y, c):
    return struct.unpack("<f", struct.pack("<f", FLOAT_VALUES[hdr_index(x, y, c, len(FLOAT_VALUES))]))[0]


def exr_attr(name, typ, data):
    return name + b"\0" + typ + b"\0" + struct.pack("<I", len(data)) + data


def exr_chlist(channels):
    # channels: [(name, pixel type)], pixel type 0 UINT, 1 HALF, 2 FLOAT; the file lists them sorted.
    body = b""
    for name, ptype in sorted(channels):
        body += name.encode() + b"\0" + struct.pack("<iBxxxii", ptype, 0, 1, 1)
    return exr_attr(b"channels", b"chlist", body + b"\0")


def exr_bytes(channels, sample, compression=0, version=2, w=W, h=H):
    """A scanline EXR. sample(name, x, y) returns the half bits (pixel type 1), the float
    (type 2) or the integer (type 0) of channel `name` at texel (x, y), y = 0 the bottom row."""
    lines_per_block = {0: 1, 3: 16}[compression]
    head = b"\x76\x2f\x31\x01" + struct.pack("<I", version)
    head += exr_chlist(channels)
    head += exr_attr(b"compression", b"compression", bytes([compression]))
    box = struct.pack("<iiii", 0, 0, w - 1, h - 1)
    head += exr_attr(b"dataWindow", b"box2i", box) + exr_attr(b"displayWindow", b"box2i", box)
    head += exr_attr(b"lineOrder", b"lineOrder", b"\x00")  # increasing Y: top row first
    head += exr_attr(b"pixelAspectRatio", b"float", struct.pack("<f", 1.0))
    head += exr_attr(b"screenWindowCenter", b"v2f", struct.pack("<ff", 0.0, 0.0))
    head += exr_attr(b"screenWindowWidth", b"float", struct.pack("<f", 1.0))
    head += b"\0"
    ordered = sorted(channels)
    blocks = []
    for first in range(0, h, lines_per_block):
        raw = b""
        for line in range(first, min(first + lines_per_block, h)):
            y = h - 1 - line
            for name, ptype in ordered:  # per line, each channel's whole row, in channel order
                for x in range(w):
                    raw += struct.pack({0: "<I", 1: "<H", 2: "<f"}[ptype], sample(name, x, y))
        if compression == 3:  # ZIP: even bytes then odd bytes, predictor back to front, zlib
            t = bytearray(raw[0::2] + raw[1::2])
            for i in range(len(t) - 1, 0, -1):
                t[i] = (t[i] - t[i - 1] + 128) & 255
            packed = zlib.compress(bytes(t), 9)
            assert len(packed) < len(raw)  # a block that does not shrink would be stored raw
            raw = packed
        blocks.append((first, raw))
    pos = len(head) + 8 * len(blocks)
    out = head
    for _, raw in blocks:
        out += struct.pack("<Q", pos)
        pos += 8 + len(raw)
    for first, raw in blocks:
        out += struct.pack("<iI", first, len(raw)) + raw
    return out


def sample_half(name, x, y):
    return half_bits(x, y, "RGBA".index(name))


def sample_float(name, x, y):
    return float_value(x, y, "RGBA".index(name))


def hdr_bytes():
    # Flat (not run-length) RGBE: a width below 8 never uses the RLE form. The mantissa bytes are
    # the 8-bit pattern; the exponent byte varies per texel so several scales are exercised.
    out = b"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n" % (H, W)
    for top in range(H):
        y = H - 1 - top
        for x in range(W):
            out += bytes([texel(x, y, 0), texel(x, y, 1), texel(x, y, 2), 120 + (x + 2 * y) % 5])
    return out


RGBA_H = [("R", 1), ("G", 1), ("B", 1), ("A", 1)]
RGB_H = [("R", 1), ("G", 1), ("B", 1)]
RGB_F = [("R", 2), ("G", 2), ("B", 2)]
write("pattern_4x3_half_rgba.exr", exr_bytes(RGBA_H, sample_half))
write("pattern_4x3_float_rgb.exr", exr_bytes(RGB_F, sample_float))
write("pattern_4x3_half_zip.exr", exr_bytes(RGB_H, sample_half, compression=3))
write("pattern_4x3_half_y.exr", exr_bytes([("Y", 1)], lambda n, x, y: half_bits(x, y, 0)))
write("pattern_4x3.hdr", hdr_bytes())
# One file per rejection: each must decode to a 1x1 magenta image with one IMAGE_UNREADABLE entry.
write("bad_uint_channel.exr", exr_bytes([("R", 0), ("G", 1), ("B", 1)], lambda n, x, y: 7 if n == "R" else half_bits(x, y, 0)))
write("bad_no_green.exr", exr_bytes([("R", 1), ("B", 1)], sample_half))
write("bad_nan_half.exr", exr_bytes(RGB_H, lambda n, x, y: 0x7E00 if (n, x, y) == ("G", 2, 1) else sample_half(n, x, y)))
write("bad_inf_float.exr", exr_bytes(RGB_F, lambda n, x, y: float("inf") if (n, x, y) == ("B", 3, 2) else sample_float(n, x, y)))
write("bad_deep.exr", exr_bytes(RGB_H, sample_half, version=2 | 0x800))
write("bad_multipart.exr", exr_bytes(RGB_H, sample_half, version=2 | 0x1000))
write("bad_truncated.exr", exr_bytes(RGB_H, sample_half)[:-20])

import bpy  # noqa: E402  (only inside Blender)

img = bpy.data.images.new("pattern", W, H, alpha=False)
img.colorspace_settings.name = "Non-Color"  # bytes in, bytes out: no transfer function
pix = []
for y in range(H):
    for x in range(W):
        pix += [texel(x, y, 0) / 255.0, texel(x, y, 1) / 255.0, texel(x, y, 2) / 255.0, 1.0]
img.pixels = pix
scene = bpy.context.scene
scene.render.image_settings.file_format = "JPEG"
scene.render.image_settings.color_mode = "RGB"
scene.render.image_settings.quality = 100
img.save_render(os.path.join(OUT, "pattern_4x3.jpg"), scene=scene)
print("GENERATED", sorted(os.listdir(OUT)))
