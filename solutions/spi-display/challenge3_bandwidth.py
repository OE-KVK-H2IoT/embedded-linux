#!/usr/bin/env python3
"""Solution — Challenge 3: Bandwidth Calculation

Calculates theoretical vs measured SPI display performance.
"""
import struct, time, glob, os, sys

# ── Find SPI framebuffer ─────────────────────────────────
fb_dev = None
for fb in sorted(glob.glob("/sys/class/graphics/fb*")):
    name_file = os.path.join(fb, "name")
    if os.path.exists(name_file):
        with open(name_file) as f:
            name = f.read().strip().lower()
        if "ili" in name or "fbtft" in name or "st7" in name:
            fb_dev = "/dev/" + os.path.basename(fb)
            break

if not fb_dev:
    print("No SPI framebuffer found")
    sys.exit(1)

fb_name = os.path.basename(fb_dev)
def read_sysfs(attr):
    with open(f"/sys/class/graphics/{fb_name}/{attr}") as f:
        return f.read().strip()

WIDTH, HEIGHT = [int(x) for x in read_sysfs("virtual_size").split(",")]
BPP = int(read_sysfs("bits_per_pixel"))
STRIDE = int(read_sysfs("stride"))
FB_SIZE = STRIDE * HEIGHT

SPI_CLOCK_HZ = 32_000_000  # 32 MHz — adjust if your overlay uses different speed

# ── Theoretical calculation ──────────────────────────────
frame_bits = WIDTH * HEIGHT * BPP  # only pixel data, no stride padding
frame_bytes = WIDTH * HEIGHT * (BPP // 8)

theoretical_fps = SPI_CLOCK_HZ / frame_bits
frame_time_theoretical_ms = 1000 / theoretical_fps

print("=== Theoretical Calculation ===")
print(f"Display: {WIDTH}x{HEIGHT} @ {BPP} bpp")
print(f"Frame size: {WIDTH} × {HEIGHT} × {BPP//8} = {frame_bytes:,} bytes = {frame_bits:,} bits")
print(f"SPI clock: {SPI_CLOCK_HZ / 1e6:.0f} MHz = {SPI_CLOCK_HZ:,} bits/sec")
print(f"Theoretical max FPS: {SPI_CLOCK_HZ:,} / {frame_bits:,} = {theoretical_fps:.1f} FPS")
print(f"Theoretical frame time: {frame_time_theoretical_ms:.1f} ms")

# ── Measured performance ─────────────────────────────────
FRAMES = 30
print(f"\n=== Measured Performance ({FRAMES} frames) ===")

# Pre-generate frames
colors = [(255, 0, 0), (0, 255, 0), (0, 0, 255)]
frames = []
for r, g, b in colors:
    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
    pixel = struct.pack("<H", rgb565)
    row = pixel * WIDTH + b'\x00' * (STRIDE - WIDTH * 2)
    frames.append(row * HEIGHT)

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
measured_fps = 1000 / avg_ms
measured_throughput = FB_SIZE * 8 / (avg_ms / 1000) / 1e6

print(f"Average frame time: {avg_ms:.1f} ms")
print(f"Measured FPS: {measured_fps:.1f}")
print(f"Measured throughput: {measured_throughput:.1f} Mbit/s")

# ── Analysis ─────────────────────────────────────────────
efficiency = measured_fps / theoretical_fps * 100
overhead_ms = avg_ms - frame_time_theoretical_ms

print(f"\n=== Analysis ===")
print(f"Theoretical FPS:  {theoretical_fps:.1f}")
print(f"Measured FPS:     {measured_fps:.1f}")
print(f"Efficiency:       {efficiency:.0f}%")
print(f"Overhead per frame: {overhead_ms:.1f} ms")
print()
print("Why is measured lower than theoretical?")
print("  1. SPI protocol overhead — chip select, command bytes, address window setup")
print("  2. Kernel scheduling — context switches, interrupt handling, DMA setup")
print("  3. Python overhead — fb.write() → kernel copy → DMA → SPI")
print("  4. Stride padding — extra bytes sent per row if stride > width × 2")
print("  5. fbtft dirty-region tracking — driver overhead per update")
