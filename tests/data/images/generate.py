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
