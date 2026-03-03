#!/usr/bin/env python3
"""Solution — Challenge 1: Partial Updates

Compares full-frame vs partial-region updates on the SPI display.
Draws a status UI, then updates only the text area repeatedly,
measuring the speedup from partial writes.
"""
import struct, time, glob, os, sys

# ── Find SPI framebuffer ─────────────────────────────────
fb_dev = None
for fb in sorted(glob.glob("/sys/class/graphics/fb*")):
    name_file = os.path.join(fb, "name")
    if os.path.exists(name_file):
        with open(name_file) as f:
            name = f.read().strip()
        if "ili" in name.lower() or "fbtft" in name.lower() or "st7" in name.lower():
            fb_dev = "/dev/" + os.path.basename(fb)
            print(f"Found SPI display: {fb_dev} ({name})")
            break

if not fb_dev:
    print("No SPI framebuffer found")
    sys.exit(1)

fb_name = os.path.basename(fb_dev)
def read_sysfs(attr):
    with open(f"/sys/class/graphics/{fb_name}/{attr}") as f:
        return f.read().strip()

WIDTH, HEIGHT = [int(x) for x in read_sysfs("virtual_size").split(",")]
STRIDE = int(read_sysfs("stride"))
FB_SIZE = STRIDE * HEIGHT

print(f"Display: {WIDTH}x{HEIGHT}, stride={STRIDE}, frame={FB_SIZE} bytes")

# ── PIL drawing ──────────────────────────────────────────
from PIL import Image, ImageDraw, ImageFont

def rgb565_be(r, g, b):
    return struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

def image_to_rgb565(img, width, height, stride):
    """Convert PIL image to big-endian RGB565 bytearray."""
    raw = bytearray(stride * height)
    for y in range(height):
        for x in range(width):
            r, g, b = img.getpixel((x, y))
            offset = y * stride + x * 2
            struct.pack_into("<H", raw, offset, ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return raw

def region_to_rgb565(img, x0, y0, x1, y1, stride):
    """Convert a rectangular region to RGB565 rows."""
    rows = bytearray()
    for y in range(y0, y1):
        row = bytearray(stride)
        for x in range(x0, min(x1, img.width)):
            r, g, b = img.getpixel((x, y))
            offset = x * 2
            struct.pack_into("<H", row, offset, ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
        rows += row
    return rows

# ── Measure full-frame writes ────────────────────────────
ITERATIONS = 10

print(f"\n--- Full-frame update ({ITERATIONS} iterations) ---")
times_full = []
for i in range(ITERATIONS):
    img = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 20))
    draw = ImageDraw.Draw(img)
    draw.rectangle([10, 10, WIDTH-10, HEIGHT-10], outline=(0, 200, 255), width=2)
    draw.text((30, 30), f"Frame #{i}", fill=(110, 110, 120))
    draw.text((30, 80), f"Counter: {i * 42}", fill=(40, 110, 50))

    raw = image_to_rgb565(img, WIDTH, HEIGHT, STRIDE)

    t0 = time.monotonic()
    with open(fb_dev, "wb") as fb:
        fb.write(raw)
    t1 = time.monotonic()
    times_full.append(t1 - t0)

avg_full = sum(times_full) / len(times_full) * 1000
print(f"Average full-frame time: {avg_full:.1f} ms")
print(f"Full frame size: {FB_SIZE} bytes")

# ── Measure partial updates (only 50 rows) ───────────────
UPDATE_Y0 = 20   # start row of the text region
UPDATE_Y1 = 120  # end row (50–100 rows of text)
UPDATE_ROWS = UPDATE_Y1 - UPDATE_Y0

print(f"\n--- Partial update ({UPDATE_ROWS} rows, {ITERATIONS} iterations) ---")

# Draw the static background once
bg = Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 20))
draw_bg = ImageDraw.Draw(bg)
draw_bg.rectangle([10, 10, WIDTH-10, HEIGHT-10], outline=(0, 200, 255), width=2)
raw_bg = image_to_rgb565(bg, WIDTH, HEIGHT, STRIDE)
with open(fb_dev, "wb") as fb:
    fb.write(raw_bg)

times_partial = []
for i in range(ITERATIONS):
    # Redraw only the text region
    img = bg.copy()
    draw = ImageDraw.Draw(img)
    draw.text((30, 30), f"Frame #{i}", fill=(110, 110, 120))
    draw.text((30, 80), f"Counter: {i * 42}", fill=(40, 110, 50))

    # Extract only the changed rows
    partial_data = region_to_rgb565(img, 0, UPDATE_Y0, WIDTH, UPDATE_Y1, STRIDE)
    write_offset = UPDATE_Y0 * STRIDE

    t0 = time.monotonic()
    with open(fb_dev, "r+b") as fb:
        fb.seek(write_offset)
        fb.write(partial_data)
    t1 = time.monotonic()
    times_partial.append(t1 - t0)

avg_partial = sum(times_partial) / len(times_partial) * 1000
partial_bytes = UPDATE_ROWS * STRIDE

print(f"Average partial time: {avg_partial:.1f} ms")
print(f"Partial size: {partial_bytes} bytes ({UPDATE_ROWS} rows)")

# ── Comparison ───────────────────────────────────────────
print(f"\n--- Comparison ---")
print(f"Full frame:    {avg_full:.1f} ms  ({FB_SIZE} bytes)")
print(f"Partial ({UPDATE_ROWS} rows): {avg_partial:.1f} ms  ({partial_bytes} bytes)")
if avg_partial > 0:
    speedup = avg_full / avg_partial
    print(f"Speedup:       {speedup:.1f}x faster")
    print(f"Data ratio:    {partial_bytes / FB_SIZE:.1%} of full frame")
print(f"\nConclusion: Partial updates are roughly proportional to the")
print(f"number of rows written — update only what changes!")
