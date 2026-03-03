#!/usr/bin/env python3
"""Solution — Task B: Draw a Gradient

Draws a horizontal gradient (black → blue) with three approaches:
1. Row-at-a-time (slow)
2. Precompute once (fast)
3. Single write (fastest)

Also demonstrates dithering vs non-dithered, split top/bottom.
"""
import mmap, struct, time, random, sys

# ── Read framebuffer parameters from sysfs ────────────────────
def read_sysfs(name):
    with open(f"/sys/class/graphics/fb0/{name}") as f:
        return f.read().strip()

width, height = [int(x) for x in read_sysfs("virtual_size").split(",")]
bpp = int(read_sysfs("bits_per_pixel"))
stride = int(read_sysfs("stride"))
fb_size = stride * height
pixel_bytes = bpp // 8

print(f"Framebuffer: {width}x{height}, {bpp} bpp, stride={stride}")

# ── Pixel packing ─────────────────────────────────────────────
def rgb565(r, g, b):
    return struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

def xrgb8888(r, g, b):
    return struct.pack("<I", (r << 16) | (g << 8) | b)

if bpp == 16:
    pack_pixel = rgb565
elif bpp == 32:
    pack_pixel = xrgb8888
else:
    print(f"Unsupported bpp: {bpp}")
    sys.exit(1)

# ── Approach 1: Row-at-a-time (slow — rebuilds the same row each time) ──
print("\nApproach 1: Row-at-a-time...")
with open("/dev/fb0", "r+b") as f:
    mm = mmap.mmap(f.fileno(), fb_size)

    t0 = time.monotonic()
    for y in range(height):
        row = b''.join(pack_pixel(0, 0, int(255 * x / (width - 1))) for x in range(width))
        off = y * stride
        mm[off:off + width * pixel_bytes] = row
    t1 = time.monotonic()

    print(f"  Time: {(t1-t0)*1000:.0f} ms")
    mm.close()

# ── Approach 2: Precompute once (fast) ────────────────────────
print("Approach 2: Precompute once...")
with open("/dev/fb0", "r+b") as f:
    mm = mmap.mmap(f.fileno(), fb_size)

    t0 = time.monotonic()
    row = b''.join(pack_pixel(0, 0, int(255 * x / (width - 1))) for x in range(width))
    row += b'\x00' * (stride - width * pixel_bytes)  # pad to stride

    for y in range(height):
        off = y * stride
        mm[off:off + stride] = row
    t1 = time.monotonic()

    print(f"  Time: {(t1-t0)*1000:.0f} ms")
    mm.close()

# ── Approach 3: Single write (fastest) ────────────────────────
print("Approach 3: Single write...")
with open("/dev/fb0", "r+b") as f:
    mm = mmap.mmap(f.fileno(), fb_size)

    t0 = time.monotonic()
    row = b''.join(pack_pixel(0, 0, int(255 * x / (width - 1))) for x in range(width))
    row += b'\x00' * (stride - width * pixel_bytes)
    mm[:fb_size] = row * height
    t1 = time.monotonic()

    print(f"  Time: {(t1-t0)*1000:.0f} ms")
    mm.close()

# ── Dithering comparison: top half = no dither, bottom half = dithered ──
print("\nDithering comparison (top=banded, bottom=dithered)...")
with open("/dev/fb0", "r+b") as f:
    mm = mmap.mmap(f.fileno(), fb_size)

    # Top half: no dithering (shows banding)
    row_clean = b''.join(
        pack_pixel(0, 0, int(255 * x / (width - 1)))
        for x in range(width)
    )
    row_clean += b'\x00' * (stride - width * pixel_bytes)

    for y in range(height // 2):
        off = y * stride
        mm[off:off + stride] = row_clean

    # Bottom half: dithered (smoother)
    for y in range(height // 2, height):
        row_dithered = b''.join(
            pack_pixel(0, 0, max(0, min(255, int(255.0 * x / (width - 1) + random.random() - 0.5))))
            for x in range(width)
        )
        off = y * stride
        mm[off:off + width * pixel_bytes] = row_dithered

    mm.close()

print("Done — compare the top half (banded) with the bottom half (dithered).")
