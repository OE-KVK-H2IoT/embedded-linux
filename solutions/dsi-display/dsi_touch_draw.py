#!/usr/bin/env python3
"""Touch drawing app for DSI display.

Auto-detects pixel format (16bpp RGB565 or 32bpp XRGB8888) from sysfs.

Key differences from the SPI version:
  - Touch coordinates are already in screen pixels (0-799, 0-479)
    so no ADC-to-pixel calibration matrix is needed
  - Uses multitouch protocol B (ABS_MT_POSITION_X/Y) instead of
    single-touch ABS_X/ABS_Y with ADC values

Usage: sudo python3 dsi_touch_draw.py
"""
import struct, os, sys, time

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

WIDTH, HEIGHT = [int(x) for x in read_sysfs("virtual_size").split(",")]
STRIDE = int(read_sysfs("stride"))
bpp = int(read_sysfs("bits_per_pixel"))
Bpp = bpp // 8  # bytes per pixel
print(f"Display: {fb_dev} ({WIDTH}x{HEIGHT}, stride={STRIDE}, {bpp}bpp)")

# ── Find capacitive touch device ─────────────────────────
import evdev

touch_dev = None
for path in evdev.list_devices():
    dev = evdev.InputDevice(path)
    # Look for FT5x06, Goodix, or any device with INPUT_PROP_DIRECT
    caps = dev.capabilities()
    if evdev.ecodes.EV_ABS in caps:
        props = dev.capabilities().get(0, [])
        # Check for common capacitive touch names
        name_lower = dev.name.lower()
        if any(k in name_lower for k in ("ft5", "goodix", "gt9", "touch", "cap")):
            touch_dev = dev
            break
        # Or check for INPUT_PROP_DIRECT (touchscreen, not trackpad)
        if hasattr(evdev.ecodes, 'INPUT_PROP_DIRECT'):
            try:
                props = dev.input_props()
                if 1 in props:  # INPUT_PROP_DIRECT
                    touch_dev = dev
                    break
            except AttributeError:
                pass

if not touch_dev:
    print("No touch device found. Check: sudo evtest")
    sys.exit(1)

# Read coordinate ranges — capacitive touch reports pixel coordinates
caps = touch_dev.capabilities(absinfo=True)
abs_info = {code: info for code, info in caps.get(evdev.ecodes.EV_ABS, [])}

# Prefer multitouch axes (ABS_MT_POSITION_X/Y), fall back to ABS_X/Y
if evdev.ecodes.ABS_MT_POSITION_X in abs_info:
    x_info = abs_info[evdev.ecodes.ABS_MT_POSITION_X]
    y_info = abs_info[evdev.ecodes.ABS_MT_POSITION_Y]
    use_mt = True
    print(f"Touch: {touch_dev.name} (multitouch protocol B)")
else:
    x_info = abs_info[evdev.ecodes.ABS_X]
    y_info = abs_info[evdev.ecodes.ABS_Y]
    use_mt = False
    print(f"Touch: {touch_dev.name} (single-touch)")

print(f"  X range: {x_info.min}..{x_info.max}")
print(f"  Y range: {y_info.min}..{y_info.max}")

# ── Coordinate mapping ───────────────────────────────────
# Capacitive touch on DSI: coordinates are already pixel-aligned.
# No SWAP_XY/INVERT calibration needed (unlike SPI resistive touch).
def map_touch(raw_x, raw_y):
    """Map touch coordinates to screen pixels. Direct mapping for capacitive."""
    px = int(raw_x * (WIDTH - 1) / x_info.max) if x_info.max > 0 else raw_x
    py = int(raw_y * (HEIGHT - 1) / y_info.max) if y_info.max > 0 else raw_y
    return max(0, min(WIDTH-1, px)), max(0, min(HEIGHT-1, py))

# ── Drawing state ────────────────────────────────────────
COLORS = [
    (255, 255, 255),
    (255,   0,   0),
    (  0, 255,   0),
    (  0, 100, 255),
    (255, 255,   0),
    (255,   0, 255),
]
color_idx = 0
BRUSH_SIZE = 5  # larger brush since DSI has more pixels

BTN_SIZE = 50
BTN_CLEAR = (0, 0, BTN_SIZE, BTN_SIZE)
BTN_COLOR = (WIDTH - BTN_SIZE, 0, WIDTH, BTN_SIZE)

def pack_pixel(r, g, b):
    """Pack RGB into the detected framebuffer pixel format."""
    if bpp == 16:
        return struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
    return struct.pack("<I", 0xFF000000 | (r << 16) | (g << 8) | b)

# ── Canvas ───────────────────────────────────────────────
canvas = bytearray(STRIDE * HEIGHT)

def draw_ui():
    """Draw button indicators and border."""
    border = pack_pixel(40, 40, 40)
    # Top and bottom edges
    for x in range(WIDTH):
        for row in [0, HEIGHT - 1]:
            off = row * STRIDE + x * Bpp
            canvas[off:off+Bpp] = border
    # Left and right edges
    for y in range(HEIGHT):
        for col_off in [0, (WIDTH - 1) * Bpp]:
            off = y * STRIDE + col_off
            canvas[off:off+Bpp] = border

    # Clear button (top-left) — red
    clear_px = pack_pixel(180, 0, 0)
    for y in range(BTN_SIZE):
        for x in range(BTN_SIZE):
            off = y * STRIDE + x * Bpp
            canvas[off:off+Bpp] = clear_px
    # "C" text
    white_px = pack_pixel(255, 255, 255)
    for y in range(12, 38):
        for x in range(14, 19):
            off = y * STRIDE + x * Bpp
            canvas[off:off+Bpp] = white_px
    for x in range(14, 36):
        for y_off in [12, 37]:
            off = y_off * STRIDE + x * Bpp
            canvas[off:off+Bpp] = white_px

    # Color button (top-right) — current color
    r, g, b = COLORS[color_idx]
    color_px = pack_pixel(r, g, b)
    for y in range(BTN_SIZE):
        for x in range(WIDTH - BTN_SIZE, WIDTH):
            off = y * STRIDE + x * Bpp
            canvas[off:off+Bpp] = color_px

draw_ui()

def flush_full(fb_file):
    fb_file.seek(0)
    fb_file.write(canvas)

def flush_rows(fb_file, y_min, y_max):
    y_min = max(0, y_min)
    y_max = min(HEIGHT - 1, y_max)
    offset = y_min * STRIDE
    length = (y_max - y_min + 1) * STRIDE
    fb_file.seek(offset)
    fb_file.write(canvas[offset:offset + length])

# ── Drawing primitives ───────────────────────────────────
def draw_dot(cx, cy, radius, color_rgb):
    r, g, b = color_rgb
    pixel = pack_pixel(r, g, b)
    y_min = max(0, cy - radius)
    y_max = min(HEIGHT - 1, cy + radius)
    for dy in range(-radius, radius + 1):
        py = cy + dy
        if py < 0 or py >= HEIGHT:
            continue
        dx_max = int((radius**2 - dy**2) ** 0.5)
        x_start = max(0, cx - dx_max)
        x_end = min(WIDTH - 1, cx + dx_max)
        for px in range(x_start, x_end + 1):
            off = py * STRIDE + px * Bpp
            canvas[off:off+Bpp] = pixel
    return y_min, y_max

def draw_line(x0, y0, x1, y1, color_rgb):
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

def in_rect(px, py, rect):
    return rect[0] <= px <= rect[2] and rect[1] <= py <= rect[3]

# ── Main event loop ──────────────────────────────────────
print(f"\nColor: {COLORS[color_idx]} | Brush: {BRUSH_SIZE}px")
print("Top-left = CLEAR | Top-right = CHANGE COLOR")
print("Touch to draw. Ctrl+C to quit.")

raw_x, raw_y = 0, 0
prev_px, prev_py = None, None
touching = False
event_count = 0
total_latency_ms = 0.0

try:
    fb_file = open(fb_dev, "wb")
    flush_full(fb_file)

    for event in touch_dev.read_loop():
        if event.type == evdev.ecodes.EV_ABS:
            if use_mt:
                if event.code == evdev.ecodes.ABS_MT_POSITION_X:
                    raw_x = event.value
                elif event.code == evdev.ecodes.ABS_MT_POSITION_Y:
                    raw_y = event.value
            else:
                if event.code == evdev.ecodes.ABS_X:
                    raw_x = event.value
                elif event.code == evdev.ecodes.ABS_Y:
                    raw_y = event.value

        elif event.type == evdev.ecodes.EV_KEY:
            if event.code == evdev.ecodes.BTN_TOUCH:
                touching = (event.value == 1)
                if not touching:
                    prev_px, prev_py = None, None

        elif event.type == evdev.ecodes.EV_SYN:
            if not touching:
                continue

            t_start = time.monotonic()
            px, py = map_touch(raw_x, raw_y)

            # Clear button
            if in_rect(px, py, BTN_CLEAR):
                canvas[:] = b'\x00' * len(canvas)
                draw_ui()
                flush_full(fb_file)
                prev_px, prev_py = None, None
                continue

            # Color button
            if in_rect(px, py, BTN_COLOR):
                color_idx = (color_idx + 1) % len(COLORS)
                print(f"Color: {COLORS[color_idx]}")
                r, g, b = COLORS[color_idx]
                color_px = pack_pixel(r, g, b)
                for yy in range(BTN_SIZE):
                    for xx in range(WIDTH - BTN_SIZE, WIDTH):
                        off = yy * STRIDE + xx * Bpp
                        canvas[off:off+Bpp] = color_px
                flush_rows(fb_file, 0, BTN_SIZE)
                prev_px, prev_py = None, None
                continue

            # Draw
            if prev_px is not None:
                y_min, y_max = draw_line(prev_px, prev_py, px, py, COLORS[color_idx])
            else:
                y_min, y_max = draw_dot(px, py, BRUSH_SIZE, COLORS[color_idx])

            flush_rows(fb_file, y_min, y_max)
            prev_px, prev_py = px, py

            # Latency tracking
            t_end = time.monotonic()
            latency_ms = (t_end - t_start) * 1000
            event_count += 1
            total_latency_ms += latency_ms

            if event_count % 100 == 0:
                avg = total_latency_ms / event_count
                print(f"[{event_count} events] avg latency: {avg:.1f} ms | "
                      f"last: {latency_ms:.1f} ms")

except KeyboardInterrupt:
    if event_count > 0:
        print(f"\nFinal: {event_count} events, avg latency {total_latency_ms/event_count:.1f} ms")
    print("Done.")
finally:
    fb_file.close()
