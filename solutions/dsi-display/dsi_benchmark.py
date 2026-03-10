#!/usr/bin/env python3
"""Measure DSI framebuffer write speed.

Writes solid-color frames to the DRM framebuffer and measures timing.
Auto-detects pixel format (16bpp RGB565 or 32bpp XRGB8888) from sysfs.
Unlike SPI where the bus is the bottleneck (~13 FPS), the DSI/DRM
framebuffer write is memory-to-memory — the GPU scans it out independently.

Usage: sudo python3 dsi_benchmark.py
"""
import struct, time, os, sys

# ── Find DRM framebuffer ─────────────────────────────────
fb_dev = None
for fb_name in sorted(os.listdir("/sys/class/graphics/")):
    if not fb_name.startswith("fb"):
        continue
    name_file = f"/sys/class/graphics/{fb_name}/name"
    if os.path.exists(name_file):
        with open(name_file) as f:
            name = f.read().strip()
        if not any(k in name.lower() for k in ("ili", "fbtft", "st7")):
            fb_dev = f"/dev/{fb_name}"
            break

if not fb_dev:
    fb_dev = "/dev/fb0"

fb_id = os.path.basename(fb_dev)
def read_sysfs(attr):
    with open(f"/sys/class/graphics/{fb_id}/{attr}") as f:
        return f.read().strip()

width, height = [int(x) for x in read_sysfs("virtual_size").split(",")]
stride = int(read_sysfs("stride"))
bpp = int(read_sysfs("bits_per_pixel"))
Bpp = bpp // 8  # bytes per pixel
fb_size = stride * height

print(f"Display: {width}x{height}, stride={stride}, {bpp}bpp, frame={fb_size:,} bytes")
print(f"Device: {fb_dev}")

# ── Pre-generate solid-color frames ───────────────────────
FRAMES = 60
colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255)]
frames = []
for r, g, b in colors:
    if bpp == 16:
        pixel = struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    else:
        pixel = struct.pack("<I", 0xFF000000 | (r << 16) | (g << 8) | b)
    row = pixel * width + b'\x00' * (stride - width * Bpp)
    frames.append(row * height)

# ── Measure frame write times ─────────────────────────────
print(f"\nWriting {FRAMES} frames...")
times = []
with open(fb_dev, "wb") as fb:
    for i in range(FRAMES):
        t0 = time.monotonic()
        fb.seek(0)
        fb.write(frames[i % len(frames)])
        fb.flush()
        t1 = time.monotonic()
        times.append(t1 - t0)

avg_ms = sum(times) / len(times) * 1000
fps = 1000 / avg_ms
throughput = fb_size * 8 / (avg_ms / 1000) / 1e6

print(f"\nAverage frame time: {avg_ms:.1f} ms")
print(f"Effective FPS (write speed): {fps:.1f}")
print(f"Min: {min(times)*1000:.1f} ms  Max: {max(times)*1000:.1f} ms")
print(f"Write throughput: {throughput:.1f} Mbit/s")
print(f"\nNote: This measures CPU→memory write speed, not display refresh.")
print(f"The GPU scans out the framebuffer at 60 Hz independently.")
print(f"Compare with SPI: ~13 FPS due to 32 MHz SPI bus bottleneck.")
