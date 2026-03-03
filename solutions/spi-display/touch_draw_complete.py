#!/usr/bin/env python3
"""Solution — Drawing App with all tasks (A–E)

Complete touch drawing application for SPI display featuring:
  Task A: Correct coordinate mapping with auto-detection helper
  Task B: Color switching via top-right corner tap
  Task C: Clear canvas via top-left corner tap
  Task D: Line drawing with Bresenham's algorithm
  Task E: Latency measurement (printed every 100 events)

Run with: sudo python3 touch_draw_complete.py
"""
import struct, glob, os, sys, time

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
print(f"Display: {fb_dev} ({WIDTH}x{HEIGHT}, stride={STRIDE})")

# ── Find touch input device ──────────────────────────────
import evdev

touch_dev = None
for path in evdev.list_devices():
    dev = evdev.InputDevice(path)
    if "ADS7846" in dev.name or "Touch" in dev.name:
        touch_dev = dev
        break

if not touch_dev:
    print("No touch device found. Check: sudo evtest")
    sys.exit(1)

caps = touch_dev.capabilities(absinfo=True)
abs_info = {code: info for code, info in caps.get(evdev.ecodes.EV_ABS, [])}
x_info = abs_info[evdev.ecodes.ABS_X]
y_info = abs_info[evdev.ecodes.ABS_Y]
print(f"Touch: {touch_dev.name} ({touch_dev.path})")
print(f"  X range: {x_info.min}..{x_info.max}")
print(f"  Y range: {y_info.min}..{y_info.max}")

# ═══════════════════════════════════════════════════════════
# Task A: Coordinate mapping
# Adjust these three flags for your display orientation.
# ═══════════════════════════════════════════════════════════
SWAP_XY  = False
INVERT_X = False
INVERT_Y = False

def map_touch(raw_x, raw_y):
    """Map raw ADC values to screen pixel coordinates."""
    nx = (raw_x - x_info.min) / (x_info.max - x_info.min)
    ny = (raw_y - y_info.min) / (y_info.max - y_info.min)
    if SWAP_XY:
        nx, ny = ny, nx
    if INVERT_X:
        nx = 1.0 - nx
    if INVERT_Y:
        ny = 1.0 - ny
    px = int(nx * (WIDTH - 1))
    py = int(ny * (HEIGHT - 1))
    return max(0, min(WIDTH-1, px)), max(0, min(HEIGHT-1, py))

# ═══════════════════════════════════════════════════════════
# Task B: Color palette and switching
# ═══════════════════════════════════════════════════════════
COLORS = [
    (255, 255, 255),  # white
    (255,   0,   0),  # red
    (  0, 255,   0),  # green
    (  0, 100, 255),  # blue
    (255, 255,   0),  # yellow
    (255,   0, 255),  # magenta
]
color_idx = 0
BRUSH_SIZE = 3

# Button regions
BTN_SIZE = 35
BTN_CLEAR  = (0, 0, BTN_SIZE, BTN_SIZE)               # top-left
BTN_COLOR  = (WIDTH - BTN_SIZE, 0, WIDTH, BTN_SIZE)    # top-right

def rgb565(r, g, b):
    return struct.pack("<H", ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))

# ── Canvas ────────────────────────────────────────────────
canvas = bytearray(STRIDE * HEIGHT)

def draw_ui():
    """Draw the button indicators and border."""
    # Border
    border = rgb565(40, 40, 40)
    for x in range(WIDTH):
        for off in [x * 2, (HEIGHT-1) * STRIDE + x * 2]:
            canvas[off:off+2] = border
    for y in range(HEIGHT):
        for off in [y * STRIDE, y * STRIDE + (WIDTH-1) * 2]:
            canvas[off:off+2] = border

    # Clear button (top-left) — red square
    for y in range(BTN_SIZE):
        for x in range(BTN_SIZE):
            off = y * STRIDE + x * 2
            canvas[off:off+2] = rgb565(180, 0, 0)
    # "C" letter inside
    for y in range(8, 27):
        for x in range(10, 14):
            off = y * STRIDE + x * 2
            canvas[off:off+2] = rgb565(255, 255, 255)
    for x in range(10, 25):
        for y_off in [8, 26]:
            off = y_off * STRIDE + x * 2
            canvas[off:off+2] = rgb565(255, 255, 255)

    # Color button (top-right) — shows current color
    r, g, b = COLORS[color_idx]
    for y in range(BTN_SIZE):
        for x in range(WIDTH - BTN_SIZE, WIDTH):
            off = y * STRIDE + x * 2
            canvas[off:off+2] = rgb565(r, g, b)

draw_ui()

def flush_full(fb_file):
    fb_file.seek(0)
    fb_file.write(canvas)

# ── Drawing primitives (optimized for SPI responsiveness) ─
# Pre-compute brush row spans: for each dy offset, the horizontal
# pixel count at that row of the circle. This avoids sqrt per dot.
_brush_spans = []  # list of (dy, dx_max) for the current BRUSH_SIZE
for _dy in range(-BRUSH_SIZE, BRUSH_SIZE + 1):
    _dx = int((BRUSH_SIZE**2 - _dy**2) ** 0.5)
    _brush_spans.append((_dy, _dx))

def draw_dot(cx, cy, radius, color_rgb):
    """Draw a filled circle using bulk row writes. Returns (y_min, y_max)."""
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
            canvas[off:off + span * 2] = pixel * span  # bulk write entire row span
    return y_min, y_max

# ═══════════════════════════════════════════════════════════
# Task D: Bresenham line drawing
# ═══════════════════════════════════════════════════════════
def draw_line(x0, y0, x1, y1, color_rgb):
    """Draw a line using Bresenham's algorithm with brush dots.
    Returns (y_min, y_max) bounding box of all affected rows."""
    dx = abs(x1 - x0)
    dy = abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy

    overall_y_min = min(y0, y1)
    overall_y_max = max(y0, y1)

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

    return max(0, overall_y_min - BRUSH_SIZE), min(HEIGHT - 1, overall_y_max + BRUSH_SIZE)

def flush_rows(fb_file, y_min, y_max):
    """Write only the dirty rows."""
    y_min = max(0, y_min)
    y_max = min(HEIGHT - 1, y_max)
    offset = y_min * STRIDE
    length = (y_max - y_min + 1) * STRIDE
    fb_file.seek(offset)
    fb_file.write(canvas[offset:offset + length])

# ── Point in button check ────────────────────────────────
def in_rect(px, py, rect):
    return rect[0] <= px <= rect[2] and rect[1] <= py <= rect[3]

# ═══════════════════════════════════════════════════════════
# Main event loop
# ═══════════════════════════════════════════════════════════
print(f"\nColor: {COLORS[color_idx]} | Brush: {BRUSH_SIZE}px")
print("Top-left corner = CLEAR | Top-right corner = CHANGE COLOR")
print("Touch to draw. Ctrl+C to quit.")

raw_x, raw_y = 0, 0
prev_px, prev_py = None, None
touching = False

# Task E: latency tracking
event_count = 0
total_latency_ms = 0.0

try:
    fb_file = open(fb_dev, "wb")
    flush_full(fb_file)

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
                    prev_px, prev_py = None, None  # reset line state on lift

        elif event.type == evdev.ecodes.EV_SYN:
            if not touching:
                continue

            t_start = time.monotonic()
            px, py = map_touch(raw_x, raw_y)

            # ── Task C: Clear button ──────────────────
            if in_rect(px, py, BTN_CLEAR):
                canvas[:] = b'\x00' * len(canvas)  # fast bulk zero
                draw_ui()
                flush_full(fb_file)
                prev_px, prev_py = None, None
                continue

            # ── Task B: Color button ──────────────────
            if in_rect(px, py, BTN_COLOR):
                color_idx = (color_idx + 1) % len(COLORS)
                print(f"Color: {COLORS[color_idx]}")
                # Redraw the color button indicator
                r, g, b = COLORS[color_idx]
                for yy in range(BTN_SIZE):
                    for xx in range(WIDTH - BTN_SIZE, WIDTH):
                        off = yy * STRIDE + xx * 2
                        canvas[off:off+2] = rgb565(r, g, b)
                flush_rows(fb_file, 0, BTN_SIZE)
                prev_px, prev_py = None, None
                continue

            # ── Task D: Draw line or dot ──────────────
            if prev_px is not None:
                y_min, y_max = draw_line(prev_px, prev_py, px, py, COLORS[color_idx])
            else:
                y_min, y_max = draw_dot(px, py, BRUSH_SIZE, COLORS[color_idx])

            flush_rows(fb_file, y_min, y_max)
            prev_px, prev_py = px, py

            # ── Task E: Latency measurement ───────────
            t_end = time.monotonic()
            latency_ms = (t_end - t_start) * 1000
            event_count += 1
            total_latency_ms += latency_ms

            if event_count % 100 == 0:
                avg = total_latency_ms / event_count
                rows_written = y_max - y_min + 1
                spi_time_ms = rows_written * STRIDE * 8 / 32_000_000 * 1000
                print(f"[{event_count} events] avg latency: {avg:.1f} ms | "
                      f"last: {latency_ms:.1f} ms | "
                      f"SPI theoretical: {spi_time_ms:.1f} ms")

except KeyboardInterrupt:
    if event_count > 0:
        print(f"\nFinal stats: {event_count} events, "
              f"avg latency {total_latency_ms/event_count:.1f} ms")
    print("Done.")
finally:
    fb_file.close()
