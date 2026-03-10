#!/usr/bin/env python3
"""System dashboard for DSI display.

Same concept as the SPI dashboard but adapted for DSI/DRM framebuffer
and 800x480 resolution. Auto-detects pixel format (16bpp RGB565 or
32bpp XRGB8888) from sysfs. The larger screen allows a richer layout.

Usage: sudo python3 dsi_dashboard.py
"""
import struct, os, sys, time, subprocess

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
print(f"Dashboard on {fb_dev} ({WIDTH}x{HEIGHT}, {bpp}bpp)")

# ── PIL setup ────────────────────────────────────────────
from PIL import Image, ImageDraw, ImageFont

def get_font(size):
    try:
        return ImageFont.load_default(size)
    except TypeError:
        for p in ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                  "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf"]:
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
        return ImageFont.load_default()

font    = get_font(20)
font_sm = get_font(16)
font_lg = get_font(28)

# Colors — full 8-bit range available (no RGB565 posterization)
LABEL   = (140, 140, 160)
VALUE   = (200, 200, 220)
HEADER  = (60, 160, 200)
DIM     = (60, 60, 70)
BAR_BG  = (25, 25, 30)
BG      = (0, 0, 0)

BAR_OK   = (0, 150, 50)
BAR_WARN = (180, 140, 0)
BAR_CRIT = (200, 50, 50)

TXT_OK   = (60, 180, 80)
TXT_WARN = (200, 160, 40)
TXT_CRIT = (200, 60, 60)

IP_COLOR = (80, 180, 120)

def color_for_bar(pct):
    if pct < 60: return BAR_OK
    if pct < 85: return BAR_WARN
    return BAR_CRIT

def color_for_txt(pct):
    if pct < 60: return TXT_OK
    if pct < 85: return TXT_WARN
    return TXT_CRIT

# ── System data readers ──────────────────────────────────
_prev_cpu = None

def read_cpu():
    global _prev_cpu
    with open("/proc/stat") as f:
        lines = [l for l in f if l.startswith("cpu") and l[3] != " "]
    current = []
    for line in lines:
        parts = line.split()
        vals = [int(v) for v in parts[1:]]
        idle = vals[3] + (vals[4] if len(vals) > 4 else 0)
        total = sum(vals)
        current.append((idle, total))
    if _prev_cpu is None:
        _prev_cpu = current
        return [0.0] * len(current)
    pcts = []
    for (pi, pt), (ci, ct) in zip(_prev_cpu, current):
        d_total = ct - pt
        d_idle = ci - pi
        pcts.append(100.0 * (1 - d_idle / d_total) if d_total > 0 else 0)
    _prev_cpu = current
    return pcts

def read_memory():
    info = {}
    with open("/proc/meminfo") as f:
        for line in f:
            parts = line.split()
            info[parts[0].rstrip(":")] = int(parts[1])
    total = info["MemTotal"] / 1024
    avail = info.get("MemAvailable", info.get("MemFree", 0)) / 1024
    return total - avail, total

def read_temp():
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            return int(f.read().strip()) / 1000
    except (FileNotFoundError, ValueError):
        return 0

def read_uptime():
    with open("/proc/uptime") as f:
        secs = int(float(f.read().split()[0]))
    h, m = divmod(secs // 60, 60)
    d, h = divmod(h, 24)
    if d > 0:
        return f"{d}d {h:02d}:{m:02d}"
    return f"{h:02d}:{m:02d}"

def read_ip():
    try:
        out = subprocess.check_output(["hostname", "-I"], timeout=2).decode().strip()
        return out.split()[0] if out else "no network"
    except Exception:
        return "no network"

def read_disk():
    st = os.statvfs("/")
    total = st.f_blocks * st.f_frsize / (1024**3)
    free = st.f_bavail * st.f_frsize / (1024**3)
    return total - free, total

# ── Rendering ────────────────────────────────────────────
def draw_bar(draw, x, y, w, h, pct):
    draw.rectangle([x, y, x + w, y + h], fill=BAR_BG)
    fill_w = int(w * min(pct, 100) / 100)
    if fill_w > 0:
        draw.rectangle([x, y, x + fill_w, y + h], fill=color_for_bar(pct))

def render_frame(cpu_pcts, mem_used, mem_total, temp, uptime, ip, disk_used, disk_total):
    img = Image.new("RGB", (WIDTH, HEIGHT), BG)
    draw = ImageDraw.Draw(img)
    M = 10
    y = M

    # Header
    draw.text((M, y), "SYSTEM MONITOR", fill=HEADER, font=font_lg)
    draw.text((WIDTH - 100, y + 4), uptime, fill=DIM, font=font)
    y += 34
    draw.line([(M, y), (WIDTH - M, y)], fill=DIM)
    y += 8

    # CPU bars — use two columns on wider display
    draw.text((M, y), "CPU", fill=LABEL, font=font)
    y += 24
    col_w = (WIDTH - M * 3) // 2
    bar_w = col_w - 60

    for i, pct in enumerate(cpu_pcts):
        col = i % 2
        row = i // 2
        bx = M + col * (col_w + M)
        by = y + row * 22
        draw.text((bx, by), f"{i}:", fill=DIM, font=font_sm)
        draw_bar(draw, bx + 24, by + 2, bar_w, 14, pct)
        draw.text((bx + bar_w + 28, by), f"{pct:3.0f}%", fill=color_for_txt(pct), font=font_sm)

    rows_needed = (len(cpu_pcts) + 1) // 2
    y += rows_needed * 22 + 8

    # Memory
    draw.line([(M, y), (WIDTH - M, y)], fill=DIM)
    y += 8
    mem_pct = 100 * mem_used / mem_total if mem_total > 0 else 0
    draw.text((M, y), "MEMORY", fill=LABEL, font=font)
    draw.text((WIDTH - 160, y), f"{mem_used:.0f} / {mem_total:.0f} MB", fill=VALUE, font=font_sm)
    y += 24
    draw_bar(draw, M, y, WIDTH - M * 2, 16, mem_pct)
    y += 24

    # Temperature
    draw.line([(M, y), (WIDTH - M, y)], fill=DIM)
    y += 8
    temp_pct = temp / 85 * 100
    draw.text((M, y), "TEMPERATURE", fill=LABEL, font=font)
    draw.text((WIDTH - 100, y), f"{temp:.1f} \u00b0C", fill=color_for_txt(temp_pct), font=font)
    y += 24
    draw_bar(draw, M, y, WIDTH - M * 2, 12, temp_pct)
    y += 20

    # Disk
    draw.line([(M, y), (WIDTH - M, y)], fill=DIM)
    y += 8
    disk_pct = 100 * disk_used / disk_total if disk_total > 0 else 0
    draw.text((M, y), "DISK", fill=LABEL, font=font)
    draw.text((WIDTH - 160, y), f"{disk_used:.1f} / {disk_total:.1f} GB", fill=VALUE, font=font_sm)
    y += 24
    draw_bar(draw, M, y, WIDTH - M * 2, 12, disk_pct)
    y += 20

    # Network
    draw.line([(M, y), (WIDTH - M, y)], fill=DIM)
    y += 8
    draw.text((M, y), "IP", fill=LABEL, font=font)
    draw.text((40, y), ip, fill=IP_COLOR, font=font)

    return img

def image_to_fb(img):
    """Convert PIL image to framebuffer bytearray (auto-detects pixel format)."""
    raw = bytearray(STRIDE * HEIGHT)
    pixels = img.load()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r, g, b = pixels[x, y]
            offset = y * STRIDE + x * Bpp
            if bpp == 16:
                struct.pack_into("<H", raw, offset,
                                 ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
            else:
                struct.pack_into("<I", raw, offset,
                                 0xFF000000 | (r << 16) | (g << 8) | b)
    return raw

# ── Main loop ────────────────────────────────────────────
print("Press Ctrl+C to stop.\n")

ip = read_ip()
disk_used, disk_total = read_disk()
slow_counter = 0
prev_raw = None

try:
    with open(fb_dev, "wb") as fb:
        while True:
            cpu = read_cpu()
            mem_used, mem_total = read_memory()
            temp = read_temp()
            uptime = read_uptime()

            slow_counter += 1
            if slow_counter >= 10:
                ip = read_ip()
                disk_used, disk_total = read_disk()
                slow_counter = 0

            img = render_frame(cpu, mem_used, mem_total, temp, uptime, ip, disk_used, disk_total)
            raw = image_to_fb(img)

            # Partial update: only write rows that changed
            if prev_raw is not None:
                y_min, y_max = None, None
                for y in range(HEIGHT):
                    off = y * STRIDE
                    if raw[off:off + STRIDE] != prev_raw[off:off + STRIDE]:
                        if y_min is None:
                            y_min = y
                        y_max = y
                if y_min is not None:
                    offset = y_min * STRIDE
                    length = (y_max - y_min + 1) * STRIDE
                    fb.seek(offset)
                    fb.write(raw[offset:offset + length])
            else:
                fb.seek(0)
                fb.write(raw)

            prev_raw = raw
            time.sleep(1)

except KeyboardInterrupt:
    print("\nStopped.")
