#!/usr/bin/env python3
"""System dashboard for SPI display.

Reads CPU, memory, temperature, and network info from /proc and sysfs.
Renders to the SPI framebuffer using PIL. Updates every second.
Only redraws the rows that changed (partial update).

This is the typical use case for small SPI displays in embedded products:
status panels, industrial HMIs, server rack monitors.

Usage: sudo python3 spi_dashboard.py
"""
import struct, glob, os, sys, time, subprocess

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
    print("No SPI framebuffer found. Check dmesg | grep fbtft")
    sys.exit(1)

fb_name = os.path.basename(fb_dev)
def read_sysfs(attr):
    with open(f"/sys/class/graphics/{fb_name}/{attr}") as f:
        return f.read().strip()

WIDTH, HEIGHT = [int(x) for x in read_sysfs("virtual_size").split(",")]
STRIDE = int(read_sysfs("stride"))
print(f"Dashboard on {fb_dev} ({WIDTH}x{HEIGHT})")

# ── PIL setup ────────────────────────────────────────────
from PIL import Image, ImageDraw, ImageFont

# Font setup — Pillow >= 10.1 has a scalable built-in font.
# Older versions fall back to a bold TTF (bold reduces anti-aliasing fuzziness on RGB565).
def get_font(size):
    try:
        return ImageFont.load_default(size)
    except TypeError:
        for p in ["/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                  "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf"]:
            if os.path.exists(p):
                return ImageFont.truetype(p, size)
        return ImageFont.load_default()

font    = get_font(16)
font_sm = get_font(14)
font_lg = get_font(20)

# Colors — keep ALL text brightness below ~120 to avoid posterization artifacts.
# RGB565 has only 5-6-5 bits per channel (step size 8/4/8). Anti-aliased text
# produces gray edge pixels that snap to discrete steps, creating a visible dot
# pattern at higher brightness. Below ~120 our eye's logarithmic response hides it.
LABEL   = (80, 80, 90)      # section labels
VALUE   = (110, 110, 120)   # text values
HEADER  = (40, 100, 115)    # title: teal
DIM     = (45, 45, 50)      # separators
BAR_BG  = (20, 20, 25)      # bar background
BG      = (0, 0, 0)

BAR_OK   = (0, 100, 35)     # green
BAR_WARN = (115, 90, 0)     # amber
BAR_CRIT = (120, 30, 30)    # red

TXT_OK   = (40, 110, 50)
TXT_WARN = (115, 100, 25)
TXT_CRIT = (120, 40, 40)

IP_COLOR = (50, 110, 70)

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
    """Read per-core CPU usage from /proc/stat. Returns list of percentages."""
    global _prev_cpu
    with open("/proc/stat") as f:
        lines = [l for l in f if l.startswith("cpu") and l[3] != " "]

    current = []
    for line in lines:
        parts = line.split()
        vals = [int(v) for v in parts[1:]]
        idle = vals[3] + (vals[4] if len(vals) > 4 else 0)  # idle + iowait
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
    """Return (used_mb, total_mb) from /proc/meminfo."""
    info = {}
    with open("/proc/meminfo") as f:
        for line in f:
            parts = line.split()
            info[parts[0].rstrip(":")] = int(parts[1])
    total = info["MemTotal"] / 1024
    avail = info.get("MemAvailable", info.get("MemFree", 0)) / 1024
    return total - avail, total

def read_temp():
    """Return CPU temperature in °C."""
    try:
        with open("/sys/class/thermal/thermal_zone0/temp") as f:
            return int(f.read().strip()) / 1000
    except (FileNotFoundError, ValueError):
        return 0

def read_uptime():
    """Return uptime string."""
    with open("/proc/uptime") as f:
        secs = int(float(f.read().split()[0]))
    h, m = divmod(secs // 60, 60)
    d, h = divmod(h, 24)
    if d > 0:
        return f"{d}d {h:02d}:{m:02d}"
    return f"{h:02d}:{m:02d}"

def read_ip():
    """Return first non-localhost IP address."""
    try:
        out = subprocess.check_output(["hostname", "-I"], timeout=2).decode().strip()
        return out.split()[0] if out else "no network"
    except Exception:
        return "no network"

def read_disk():
    """Return (used_gb, total_gb) for root filesystem."""
    st = os.statvfs("/")
    total = st.f_blocks * st.f_frsize / (1024**3)
    free = st.f_bavail * st.f_frsize / (1024**3)
    return total - free, total

# ── Rendering ────────────────────────────────────────────
def draw_bar(draw, x, y, w, h, pct):
    """Draw a horizontal progress bar with auto-colored fill."""
    draw.rectangle([x, y, x + w, y + h], fill=BAR_BG)
    fill_w = int(w * min(pct, 100) / 100)
    if fill_w > 0:
        draw.rectangle([x, y, x + fill_w, y + h], fill=color_for_bar(pct))

def render_frame(cpu_pcts, mem_used, mem_total, temp, uptime, ip, disk_used, disk_total):
    """Render the dashboard to a PIL Image."""
    img = Image.new("RGB", (WIDTH, HEIGHT), BG)
    draw = ImageDraw.Draw(img)
    M = 4           # margin
    y = M

    # ── Header ──
    draw.text((M, y), "SYS MONITOR", fill=HEADER, font=font_lg)
    draw.text((WIDTH - 55, y + 1), uptime, fill=DIM, font=font_sm)
    y += 21
    draw.line([(M, y), (WIDTH - M, y)], fill=DIM)
    y += 4

    # ── CPU bars ──
    draw.text((M, y), "CPU", fill=LABEL, font=font_sm)
    y += 15
    bar_w = WIDTH - 50 - M
    for i, pct in enumerate(cpu_pcts):
        draw.text((M, y), f"{i}:", fill=DIM, font=font_sm)
        draw_bar(draw, 22, y + 1, bar_w, 10, pct)
        draw.text((bar_w + 26, y), f"{pct:3.0f}%", fill=color_for_txt(pct), font=font_sm)
        y += 14
    y += 3

    # ── Memory ──
    draw.line([(M, y), (WIDTH - M, y)], fill=DIM)
    y += 4
    mem_pct = 100 * mem_used / mem_total if mem_total > 0 else 0
    draw.text((M, y), "MEM", fill=LABEL, font=font_sm)
    y += 1
    draw.text((WIDTH - 95, y), f"{mem_used:.0f}/{mem_total:.0f}M", fill=VALUE, font=font_sm)
    y += 15
    draw_bar(draw, M, y, WIDTH - M * 2, 10, mem_pct)
    y += 15

    # ── Temperature ──
    draw.line([(M, y), (WIDTH - M, y)], fill=DIM)
    y += 4
    temp_pct = temp / 85 * 100   # 85°C = thermal throttle
    draw.text((M, y), "TEMP", fill=LABEL, font=font_sm)
    y += 1
    draw.text((WIDTH - 60, y), f"{temp:.1f} C", fill=color_for_txt(temp_pct), font=font_sm)
    y += 15
    draw_bar(draw, M, y, WIDTH - M * 2, 8, temp_pct)
    y += 12

    # ── Disk ──
    draw.line([(M, y), (WIDTH - M, y)], fill=DIM)
    y += 4
    disk_pct = 100 * disk_used / disk_total if disk_total > 0 else 0
    draw.text((M, y), "DISK", fill=LABEL, font=font_sm)
    y += 1
    draw.text((WIDTH - 100, y), f"{disk_used:.1f}/{disk_total:.1f}G", fill=VALUE, font=font_sm)
    y += 15
    draw_bar(draw, M, y, WIDTH - M * 2, 8, disk_pct)
    y += 12

    # ── Network ──
    draw.line([(M, y), (WIDTH - M, y)], fill=DIM)
    y += 3
    draw.text((M, y), "IP", fill=LABEL, font=font_sm)
    draw.text((28, y), ip, fill=IP_COLOR, font=font_sm)

    return img

def image_to_fb(img):
    """Convert PIL image to RGB565 bytearray for fbtft framebuffer."""
    raw = bytearray(STRIDE * HEIGHT)
    pixels = img.load()
    for y in range(HEIGHT):
        for x in range(WIDTH):
            r, g, b = pixels[x, y]
            struct.pack_into("<H", raw, y * STRIDE + x * 2,
                             ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3))
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

            # Refresh slow-changing data every 10 seconds
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
