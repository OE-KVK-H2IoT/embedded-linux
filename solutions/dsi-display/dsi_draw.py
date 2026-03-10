#!/usr/bin/env python3
"""Draw a status UI on the DSI display via the DRM framebuffer.

Auto-detects pixel format from sysfs. The fbdev layer typically
reports RGB565 (16bpp); DRM/KMS applications (SDL2, Qt) use
XRGB8888 (32bpp) via the full DRM path.

Usage: sudo python3 dsi_draw.py
"""
import struct, os, sys

# ── Find the DRM framebuffer ─────────────────────────────
fb_dev = None
for fb_name in sorted(os.listdir("/sys/class/graphics/")):
    if not fb_name.startswith("fb"):
        continue
    name_file = f"/sys/class/graphics/{fb_name}/name"
    if os.path.exists(name_file):
        with open(name_file) as f:
            name = f.read().strip()
        # Skip SPI framebuffers (fbtft/ili/st7)
        if any(k in name.lower() for k in ("ili", "fbtft", "st7")):
            continue
        fb_dev = f"/dev/{fb_name}"
        print(f"Found DRM framebuffer: {fb_dev} ({name})")
        break

if not fb_dev:
    fb_dev = "/dev/fb0"
    print(f"Defaulting to {fb_dev}")

# ── Read framebuffer parameters from sysfs ───────────────
fb_id = os.path.basename(fb_dev)
def read_sysfs(attr):
    with open(f"/sys/class/graphics/{fb_id}/{attr}") as f:
        return f.read().strip()

width, height = [int(x) for x in read_sysfs("virtual_size").split(",")]
bpp = int(read_sysfs("bits_per_pixel"))
stride = int(read_sysfs("stride"))
Bpp = bpp // 8   # bytes per pixel
fmt = "RGB565" if bpp == 16 else "XRGB8888"

print(f"Resolution: {width}x{height}, {bpp} bpp ({fmt}), stride={stride}")

# ── Draw with PIL ────────────────────────────────────────
from PIL import Image, ImageDraw, ImageFont

img = Image.new("RGB", (width, height), (0, 0, 0))
draw = ImageDraw.Draw(img)

try:
    font = ImageFont.load_default(size=24)
except TypeError:
    for p in ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf"]:
        if os.path.exists(p):
            font = ImageFont.truetype(p, 24)
            break
    else:
        font = ImageFont.load_default()

try:
    font_sm = ImageFont.load_default(size=18)
except TypeError:
    font_sm = font

# Status UI
draw.rectangle([10, 10, width-10, height-10], outline=(0, 200, 255), width=2)
draw.text((30, 30), "DSI DISPLAY", fill=(200, 200, 220), font=font)
draw.text((30, 70), f"Resolution: {width}x{height}", fill=(100, 180, 120), font=font_sm)
draw.text((30, 100), f"Format: {fmt} ({bpp} bpp)", fill=(100, 180, 120), font=font_sm)
draw.text((30, 130), f"Stride: {stride} bytes/row", fill=(100, 180, 120), font=font_sm)
draw.text((30, 160), f"Frame size: {stride * height:,} bytes", fill=(100, 180, 120), font=font_sm)

# GPU path info
draw.text((30, 210), "GPU scan-out path (DRM/KMS)", fill=(180, 140, 80), font=font_sm)
draw.text((30, 240), "No CPU per-frame transfer needed", fill=(180, 140, 80), font=font_sm)

# Color bars at bottom
bar_h = 60
colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0),
          (255, 0, 255), (0, 255, 255), (255, 128, 0), (128, 0, 255)]
bar_w = width // len(colors)
for i, color in enumerate(colors):
    draw.rectangle([i * bar_w, height - bar_h, (i+1) * bar_w, height], fill=color)

# Gradient bar
for x in range(width - 20):
    gray = int(x / (width - 20) * 255)
    draw.line([(x + 10, height - bar_h - 30), (x + 10, height - bar_h - 10)],
              fill=(gray, gray, gray))
draw.text((30, height - bar_h - 50), "Gradient", fill=(120, 120, 130), font=font_sm)

# ── Convert and write ────────────────────────────────────
raw = bytearray(stride * height)
pixels = img.load()

for y in range(height):
    for x in range(width):
        r, g, b = pixels[x, y]
        offset = y * stride + x * Bpp
        if bpp == 16:
            struct.pack_into("<H", raw, offset,
                             ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
        else:
            struct.pack_into("<I", raw, offset,
                             0xFF000000 | (r << 16) | (g << 8) | b)

with open(fb_dev, "wb") as fb:
    fb.write(raw)

print(f"Wrote {len(raw):,} bytes to {fb_dev}")
