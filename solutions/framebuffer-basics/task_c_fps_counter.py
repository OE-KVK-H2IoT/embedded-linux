#!/usr/bin/env python3
"""Solution — Task C: Measure Frame Rate

Bouncing square with FPS counter.
Run with and without the sleep to compare CPU usage (check htop).
"""
import mmap, struct, time, signal, sys

# ── Read FB params ────────────────────────────────────────────
def read_sysfs(name):
    with open(f"/sys/class/graphics/fb0/{name}") as f:
        return f.read().strip()

width, height = [int(x) for x in read_sysfs("virtual_size").split(",")]
bpp = int(read_sysfs("bits_per_pixel"))
stride = int(read_sysfs("stride"))
fb_size = stride * height
pixel_bytes = bpp // 8

if bpp == 16:
    pack = lambda r, g, b: struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
elif bpp == 32:
    pack = lambda r, g, b: struct.pack("<I", (r << 16) | (g << 8) | b)
else:
    sys.exit(f"Unsupported bpp: {bpp}")

BLACK = pack(0, 0, 0)
WHITE = pack(255, 255, 255)

running = True
def stop(sig, frame): global running; running = False
signal.signal(signal.SIGINT, stop)

# ── Animation with FPS counter ────────────────────────────────
SQ = 60
x, y = width // 4, height // 4
dx, dy = 3, 2

# Set to False to test busy-loop behavior (100% CPU)
USE_SLEEP = True

with open("/dev/fb0", "r+b") as f:
    mm = mmap.mmap(f.fileno(), fb_size)
    mm[:fb_size] = b'\x00' * fb_size

    print(f"Bouncing {SQ}x{SQ} on {width}x{height} — sleep={'on' if USE_SLEEP else 'OFF'}")
    print("Press Ctrl+C to stop. Watch htop in another terminal.\n")

    # FPS tracking
    frame_count = 0
    last_time = time.monotonic()

    while running:
        # Erase old position
        for row in range(y, min(y + SQ, height)):
            off = row * stride + x * pixel_bytes
            mm[off:off + SQ * pixel_bytes] = BLACK * SQ

        # Move
        x += dx; y += dy
        if x <= 0 or x + SQ >= width:  dx = -dx
        if y <= 0 or y + SQ >= height: dy = -dy

        # Draw new position
        for row in range(y, min(y + SQ, height)):
            off = row * stride + x * pixel_bytes
            mm[off:off + SQ * pixel_bytes] = WHITE * SQ

        # FPS measurement
        frame_count += 1
        now = time.monotonic()
        if now - last_time >= 1.0:
            print(f"FPS: {frame_count}  (sleep={'on' if USE_SLEEP else 'OFF'})")
            frame_count = 0
            last_time = now

        if USE_SLEEP:
            time.sleep(0.016)  # ~60 fps cap

    mm[:fb_size] = b'\x00' * fb_size
    mm.close()

print("Stopped.")
