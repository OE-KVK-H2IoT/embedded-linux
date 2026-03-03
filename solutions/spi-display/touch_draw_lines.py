#!/usr/bin/env python3
"""Solution — Task D: Line drawing with Bresenham's algorithm

Minimal drawing app that draws connected lines (not scattered dots)
when you drag your finger across the SPI display.

This is the starter code + Task D only, for students who want to
see just the line-drawing improvement without the full feature set.
"""
import struct, glob, os, sys, time
import evdev

# ── Find SPI framebuffer ─────────────────────────────────
fb_dev = None
for fb in sorted(glob.glob("/sys/class/graphics/fb*")):
    name_file = os.path.join(fb, "name")
    if os.path.exists(name_file):
        with open(name_file) as f:
            name = f.read().strip()
        if "ili" in name.lower() or "fbtft" in name.lower() or "st7" in name.lower():
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
STRIDE = int(read_sysfs("stride"))
print(f"Display: {fb_dev} ({WIDTH}x{HEIGHT})")

# ── Find touch device ────────────────────────────────────
touch_dev = None
for path in evdev.list_devices():
    dev = evdev.InputDevice(path)
    if "ADS7846" in dev.name or "Touch" in dev.name:
        touch_dev = dev
        break

if not touch_dev:
    print("No touch device found")
    sys.exit(1)

caps = touch_dev.capabilities(absinfo=True)
abs_info = {code: info for code, info in caps.get(evdev.ecodes.EV_ABS, [])}
x_info = abs_info[evdev.ecodes.ABS_X]
y_info = abs_info[evdev.ecodes.ABS_Y]
print(f"Touch: {touch_dev.name}")

# ── Coordinate mapping (adjust for your display) ─────────
SWAP_XY  = False
INVERT_X = False
INVERT_Y = False

def map_touch(raw_x, raw_y):
    nx = (raw_x - x_info.min) / (x_info.max - x_info.min)
    ny = (raw_y - y_info.min) / (y_info.max - y_info.min)
    if SWAP_XY:  nx, ny = ny, nx
    if INVERT_X: nx = 1.0 - nx
    if INVERT_Y: ny = 1.0 - ny
    return (max(0, min(WIDTH-1,  int(nx * (WIDTH - 1)))),
            max(0, min(HEIGHT-1, int(ny * (HEIGHT - 1)))))

# ── Drawing ──────────────────────────────────────────────
BRUSH_SIZE = 3
COLOR = (255, 255, 255)

def rgb565(r, g, b):
    return struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

canvas = bytearray(STRIDE * HEIGHT)

# Draw border
border = rgb565(40, 40, 40)
for x in range(WIDTH):
    canvas[x*2:x*2+2] = border
    canvas[(HEIGHT-1)*STRIDE + x*2 : (HEIGHT-1)*STRIDE + x*2 + 2] = border
for y in range(HEIGHT):
    canvas[y*STRIDE:y*STRIDE+2] = border
    canvas[y*STRIDE+(WIDTH-1)*2 : y*STRIDE+(WIDTH-1)*2+2] = border

# Pre-compute brush row spans to avoid sqrt per dot
_brush_spans = []
for _dy in range(-BRUSH_SIZE, BRUSH_SIZE + 1):
    _dx = int((BRUSH_SIZE**2 - _dy**2) ** 0.5)
    _brush_spans.append((_dy, _dx))

def draw_dot(cx, cy, radius, color_rgb):
    """Draw a filled circle with bulk row writes, return (y_min, y_max)."""
    r, g, b = color_rgb
    pixel = rgb565(r, g, b)
    y_min = max(0, cy - radius)
    y_max = min(HEIGHT - 1, cy + radius)
    for dy, dx_max in _brush_spans:
        py = cy + dy
        if py < 0 or py >= HEIGHT:
            continue
        x_start = max(0, cx - dx_max)
        x_end = min(WIDTH - 1, cx + dx_max)
        span = x_end - x_start + 1
        if span > 0:
            off = py * STRIDE + x_start * 2
            canvas[off:off + span * 2] = pixel * span
    return y_min, y_max

def draw_line(x0, y0, x1, y1, color_rgb):
    """Bresenham line with brush-size dots at each point."""
    dx = abs(x1 - x0)
    dy = abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy

    y_min = min(y0, y1)
    y_max = max(y0, y1)

    while True:
        draw_dot(x0, y0, BRUSH_SIZE, color_rgb)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x0 += sx
        if e2 < dx:
            err += dx
            y0 += sy

    return max(0, y_min - BRUSH_SIZE), min(HEIGHT - 1, y_max + BRUSH_SIZE)

def flush_rows(fb_file, y_min, y_max):
    offset = y_min * STRIDE
    length = (y_max - y_min + 1) * STRIDE
    fb_file.seek(offset)
    fb_file.write(canvas[offset:offset + length])

# ── Main loop ────────────────────────────────────────────
print("Touch to draw lines. Ctrl+C to quit.")

raw_x, raw_y = 0, 0
prev_px, prev_py = None, None
touching = False

try:
    fb_file = open(fb_dev, "wb")
    fb_file.write(canvas)  # initial frame

    for event in touch_dev.read_loop():
        if event.type == evdev.ecodes.EV_ABS:
            if event.code == evdev.ecodes.ABS_X:
                raw_x = event.value
            elif event.code == evdev.ecodes.ABS_Y:
                raw_y = event.value

        elif event.type == evdev.ecodes.EV_KEY:
            if event.code == evdev.ecodes.BTN_TOUCH:
                touching = (event.value == 1)
                if not touching:
                    prev_px, prev_py = None, None

        elif event.type == evdev.ecodes.EV_SYN and touching:
            px, py = map_touch(raw_x, raw_y)

            if prev_px is not None:
                # Draw a line from previous to current position
                y_min, y_max = draw_line(prev_px, prev_py, px, py, COLOR)
            else:
                # First touch — just a dot
                y_min, y_max = draw_dot(px, py, BRUSH_SIZE, COLOR)

            flush_rows(fb_file, y_min, y_max)
            prev_px, prev_py = px, py

except KeyboardInterrupt:
    print("\nDone.")
finally:
    fb_file.close()
