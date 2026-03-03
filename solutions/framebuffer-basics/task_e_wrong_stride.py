#!/usr/bin/env python3
"""Solution — Task E: Wrong Stride Experiment

Draws the same red rectangle with three stride values:
1. Correct stride (from sysfs)
2. Wrong stride: width * pixel_bytes (ignoring padding)
3. Wrong stride: width * pixel_bytes + 64 (artificial padding)

Each is drawn in a vertical third of the screen so you can compare.
"""
import mmap, struct, sys

def read_sysfs(name):
    with open(f"/sys/class/graphics/fb0/{name}") as f:
        return f.read().strip()

width, height = [int(x) for x in read_sysfs("virtual_size").split(",")]
bpp = int(read_sysfs("bits_per_pixel"))
correct_stride = int(read_sysfs("stride"))
fb_size = correct_stride * height
pixel_bytes = bpp // 8

if bpp == 16:
    pack = lambda r, g, b: struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
elif bpp == 32:
    pack = lambda r, g, b: struct.pack("<I", (r << 16) | (g << 8) | b)
else:
    sys.exit(f"Unsupported bpp: {bpp}")

print(f"Display: {width}x{height}, {bpp} bpp")
print(f"Correct stride: {correct_stride}")
print(f"Width × pixel_bytes: {width * pixel_bytes}")
print(f"Difference (padding): {correct_stride - width * pixel_bytes} bytes/row")

# ── Draw a rectangle with a given stride value ────────────────
def draw_rect(mm, rx, ry, rw, rh, stride_used, color):
    """Draw rectangle using the given stride for offset calculation."""
    pixel = pack(*color)
    for y in range(ry, min(ry + rh, height)):
        for x in range(rx, min(rx + rw, width)):
            offset = y * stride_used + x * pixel_bytes
            if 0 <= offset < fb_size - pixel_bytes:
                mm[offset:offset + pixel_bytes] = pixel

with open("/dev/fb0", "r+b") as f:
    mm = mmap.mmap(f.fileno(), fb_size)
    mm[:fb_size] = b'\x00' * fb_size  # clear

    third = height // 3
    rw, rh = 200, 100

    # Label positions (draw colored markers in top-left of each section)
    sections = [
        ("Correct stride", 0, correct_stride, (0, 255, 0)),
        ("No padding", third, width * pixel_bytes, (255, 255, 0)),
        ("+64 padding", 2 * third, width * pixel_bytes + 64, (255, 0, 0)),
    ]

    for label, y_start, test_stride, color in sections:
        rx = width // 2 - rw // 2
        ry = y_start + (third // 2 - rh // 2)
        print(f"\n{label}: stride={test_stride}")
        print(f"  Drawing {rw}x{rh} rectangle at ({rx}, {ry})")
        draw_rect(mm, rx, ry, rw, rh, test_stride, color)

    mm.close()

print("\nDone — look at the display:")
print("  Top third (green):  correct stride — rectangle is straight")
print("  Middle (yellow):    stride too small — diagonal smear to the LEFT")
print("  Bottom (red):       stride too large — diagonal smear to the RIGHT")
print("\nNote: If your display has zero padding, top and middle look identical.")
