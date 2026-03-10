#!/usr/bin/env python3
"""
Touch overlay for Chocolate Doom — maps letterbox margin touches to
keyboard events, center-area touches to mouse events.

Architecture:
  Real touchscreen → EVIOCGRAB (exclusive) → this script
    ├─ margin touch → virtual keyboard (uinput) → KEY_UP, KEY_LEFTCTRL, etc.
    └─ center touch → virtual mouse (uinput) → ABS_X/Y + BTN_LEFT

Usage:
  sudo python3 doom_touch_overlay.py [--width W] [--height H] [--device PATH] [--debug]

  --width   Display width in pixels (default: auto-detect from DRM)
  --height  Display height in pixels (default: auto-detect from DRM)
  --device  Touch device path (default: auto-detect)
  --debug   Print every touch event with zone assignment
"""
import argparse, sys, os, time, signal
import evdev
from evdev import UInput, ecodes, AbsInfo

# ── Command line arguments ──────────────────────────────────
parser = argparse.ArgumentParser(description="Doom touch overlay")
parser.add_argument("--width", type=int, default=0, help="Display width")
parser.add_argument("--height", type=int, default=0, help="Display height")
parser.add_argument("--device", type=str, default="", help="Touch device path")
parser.add_argument("--debug", action="store_true", help="Debug output")
args = parser.parse_args()

# ── Auto-detect display resolution ──────────────────────────
DISPLAY_W = args.width
DISPLAY_H = args.height

if DISPLAY_W == 0 or DISPLAY_H == 0:
    import glob
    for modes_file in glob.glob("/sys/class/drm/card0-*/modes"):
        try:
            mode = open(modes_file).readline().strip()
            if "x" in mode:
                w, h = mode.split("x")
                DISPLAY_W = int(w)
                DISPLAY_H = int(h)
                break
        except (ValueError, FileNotFoundError):
            continue
    if DISPLAY_W == 0:
        print("ERROR: Cannot detect display resolution. Use --width and --height.")
        sys.exit(1)

print(f"Display: {DISPLAY_W}x{DISPLAY_H}")

# ── Auto-detect touch device ───────────────────────────────
def find_touch_device():
    """Find the first touchscreen device."""
    for path in evdev.list_devices():
        dev = evdev.InputDevice(path)
        caps = dev.capabilities(verbose=False)
        abs_codes = [code for code, _ in caps.get(ecodes.EV_ABS, [])]
        # Multi-touch (ABS_MT_POSITION_X) or single-touch (ABS_X)
        if ecodes.ABS_MT_POSITION_X in abs_codes or ecodes.ABS_X in abs_codes:
            return dev
        dev.close()
    return None

if args.device:
    touch = evdev.InputDevice(args.device)
else:
    touch = find_touch_device()
    if touch is None:
        print("ERROR: No touchscreen found. Use --device to specify.")
        sys.exit(1)

print(f"Touch device: {touch.name} ({touch.path})")

# ── Detect touch protocol and coordinate range ─────────────
caps = touch.capabilities(verbose=False)
abs_caps = {code: info for code, info in caps.get(ecodes.EV_ABS, [])}

use_mt = ecodes.ABS_MT_POSITION_X in abs_caps
if use_mt:
    touch_max_x = abs_caps[ecodes.ABS_MT_POSITION_X].max
    touch_max_y = abs_caps[ecodes.ABS_MT_POSITION_Y].max
    protocol = "multi-touch (MT)"
else:
    touch_max_x = abs_caps[ecodes.ABS_X].max
    touch_max_y = abs_caps[ecodes.ABS_Y].max
    protocol = "single-touch"

print(f"Protocol: {protocol}")
print(f"Touch range: 0..{touch_max_x} x 0..{touch_max_y}")

# ── 4:3 letterbox geometry ──────────────────────────────────
GAME_W = int(DISPLAY_H * 4 / 3)
MARGIN_LEFT = (DISPLAY_W - GAME_W) // 2
MARGIN_RIGHT = DISPLAY_W - MARGIN_LEFT

print(f"Display: {DISPLAY_W}x{DISPLAY_H}, margins: {MARGIN_LEFT}px each side")

if MARGIN_LEFT < 10:
    print("WARNING: Margins too narrow for buttons. Consider a wider display or lower game aspect ratio.")

# ── Button zones ────────────────────────────────────────────
ROW_COUNT = 4

def make_buttons(x0, x1):
    """Create button rectangles for one margin."""
    row_h = DISPLAY_H // ROW_COUNT
    return {i: (x0, x1, i * row_h, (i + 1) * row_h) for i in range(ROW_COUNT)}

LEFT_BUTTONS = make_buttons(0, MARGIN_LEFT)
RIGHT_BUTTONS = make_buttons(MARGIN_RIGHT, DISPLAY_W)

# Button key mappings: (side, row_index) → key code
BUTTON_KEYS = {
    ("left", 0):  ecodes.KEY_UP,        # FWD
    ("left", 1):  ecodes.KEY_DOWN,      # BACK
    ("left", 2):  ecodes.KEY_LEFTALT,   # STRAFE
    ("left", 3):  ecodes.KEY_ESC,       # ESC / menu
    ("right", 0): ecodes.KEY_LEFTCTRL,  # FIRE
    ("right", 1): ecodes.KEY_SPACE,     # USE / open doors
    ("right", 2): ecodes.KEY_ENTER,     # ENTER (menu select)
    ("right", 3): ecodes.KEY_TAB,       # MAP
}

BUTTON_NAMES = {
    ("left", 0): "FWD", ("left", 1): "BACK",
    ("left", 2): "STRAFE", ("left", 3): "ESC",
    ("right", 0): "FIRE", ("right", 1): "USE",
    ("right", 2): "ENTER", ("right", 3): "MAP",
}

def find_button(x, y):
    """Return (key_code, name) if (x, y) is in a margin button, else (None, None)."""
    for pos, (x0, x1, y0, y1) in LEFT_BUTTONS.items():
        if x0 <= x < x1 and y0 <= y < y1:
            key = BUTTON_KEYS.get(("left", pos))
            name = BUTTON_NAMES.get(("left", pos), "?")
            return key, name
    for pos, (x0, x1, y0, y1) in RIGHT_BUTTONS.items():
        if x0 <= x < x1 and y0 <= y < y1:
            key = BUTTON_KEYS.get(("right", pos))
            name = BUTTON_NAMES.get(("right", pos), "?")
            return key, name
    return None, "center"

# ── Scale touch coordinates to display pixels ──────────────
def touch_to_display(tx, ty):
    """Convert touch coordinates to display pixel coordinates."""
    x = int(tx * DISPLAY_W / touch_max_x) if touch_max_x > 0 else tx
    y = int(ty * DISPLAY_H / touch_max_y) if touch_max_y > 0 else ty
    return x, y

# ── Virtual input devices (uinput) ──────────────────────────
all_keys = list(set(BUTTON_KEYS.values()))

ui_kbd = UInput(
    {ecodes.EV_KEY: all_keys},
    name="doom-touch-overlay-kbd"
)
print(f"Virtual keyboard: {ui_kbd.device.path}")

ui_mouse = UInput(
    {
        ecodes.EV_KEY: [ecodes.BTN_LEFT],
        ecodes.EV_ABS: [
            (ecodes.ABS_X, AbsInfo(0, 0, DISPLAY_W, 0, 0, 0)),
            (ecodes.ABS_Y, AbsInfo(0, 0, DISPLAY_H, 0, 0, 0)),
        ],
    },
    name="doom-touch-overlay-mouse"
)

# ── Grab the touch device (exclusive access) ───────────────
touch.grab()

print(f"\nOverlay ready. Start Doom NOW (after this script) so SDL2")
print(f"discovers the virtual keyboard during initialization.")

# ── Multi-touch state tracking ──────────────────────────────
MAX_SLOTS = 10  # support up to 10 simultaneous touches
slots = {}  # slot_id → {"x": raw_x, "y": raw_y, "key": held_key_or_None}
current_slot = 0

# Single-touch state (fallback)
st_x, st_y = 0, 0
st_down = False
st_key = None

# ── Signal handler for clean shutdown ───────────────────────
running = True

def handle_signal(sig, frame):
    global running
    running = False

signal.signal(signal.SIGTERM, handle_signal)
signal.signal(signal.SIGINT, handle_signal)

# ── Event loop ──────────────────────────────────────────────
try:
    for event in touch.read_loop():
        if not running:
            break

        if use_mt:
            # ── Multi-touch Protocol B ──
            if event.type == ecodes.EV_ABS:
                if event.code == ecodes.ABS_MT_SLOT:
                    current_slot = event.value
                elif event.code == ecodes.ABS_MT_TRACKING_ID:
                    if event.value == -1:
                        # Finger lifted
                        if current_slot in slots:
                            held_key = slots[current_slot].get("key")
                            if held_key is not None:
                                ui_kbd.write(ecodes.EV_KEY, held_key, 0)
                                ui_kbd.syn()
                                if args.debug:
                                    print(f"  slot {current_slot}: release key")
                            else:
                                # Release mouse button
                                ui_mouse.write(ecodes.EV_KEY, ecodes.BTN_LEFT, 0)
                                ui_mouse.syn()
                            del slots[current_slot]
                    else:
                        # New finger down
                        slots[current_slot] = {"x": 0, "y": 0, "key": None}
                elif event.code == ecodes.ABS_MT_POSITION_X:
                    if current_slot in slots:
                        slots[current_slot]["x"] = event.value
                elif event.code == ecodes.ABS_MT_POSITION_Y:
                    if current_slot in slots:
                        slots[current_slot]["y"] = event.value

            elif event.type == ecodes.EV_SYN and event.code == ecodes.SYN_REPORT:
                for slot_id, slot in slots.items():
                    dx, dy = touch_to_display(slot["x"], slot["y"])
                    key, name = find_button(dx, dy)

                    if args.debug:
                        print(f"  slot {slot_id}: ({dx}, {dy}) → {name}")

                    old_key = slot.get("key")
                    if key != old_key:
                        # Release old key if any
                        if old_key is not None:
                            ui_kbd.write(ecodes.EV_KEY, old_key, 0)
                            ui_kbd.syn()

                    if key is not None:
                        # Margin button — press/hold key
                        if key != old_key:
                            ui_kbd.write(ecodes.EV_KEY, key, 1)
                            ui_kbd.syn()
                        slot["key"] = key
                    else:
                        # Center area — inject mouse position + click
                        # Map to game area coordinates
                        game_x = max(0, min(GAME_W, dx - MARGIN_LEFT))
                        game_y = dy
                        ui_mouse.write(ecodes.EV_ABS, ecodes.ABS_X, dx)
                        ui_mouse.write(ecodes.EV_ABS, ecodes.ABS_Y, dy)
                        ui_mouse.write(ecodes.EV_KEY, ecodes.BTN_LEFT, 1)
                        ui_mouse.syn()
                        slot["key"] = None

        else:
            # ── Single-touch fallback ──
            if event.type == ecodes.EV_ABS:
                if event.code == ecodes.ABS_X:
                    st_x = event.value
                elif event.code == ecodes.ABS_Y:
                    st_y = event.value

            elif event.type == ecodes.EV_KEY and event.code == ecodes.BTN_TOUCH:
                if event.value == 1:
                    st_down = True
                else:
                    # Release
                    st_down = False
                    if st_key is not None:
                        ui_kbd.write(ecodes.EV_KEY, st_key, 0)
                        ui_kbd.syn()
                        st_key = None
                    else:
                        ui_mouse.write(ecodes.EV_KEY, ecodes.BTN_LEFT, 0)
                        ui_mouse.syn()

            elif event.type == ecodes.EV_SYN and event.code == ecodes.SYN_REPORT:
                if st_down:
                    dx, dy = touch_to_display(st_x, st_y)
                    key, name = find_button(dx, dy)

                    if args.debug:
                        print(f"  touch: ({dx}, {dy}) → {name}")

                    if key is not None:
                        if key != st_key:
                            if st_key is not None:
                                ui_kbd.write(ecodes.EV_KEY, st_key, 0)
                                ui_kbd.syn()
                            ui_kbd.write(ecodes.EV_KEY, key, 1)
                            ui_kbd.syn()
                            st_key = key
                    else:
                        if st_key is not None:
                            ui_kbd.write(ecodes.EV_KEY, st_key, 0)
                            ui_kbd.syn()
                            st_key = None
                        ui_mouse.write(ecodes.EV_ABS, ecodes.ABS_X, dx)
                        ui_mouse.write(ecodes.EV_ABS, ecodes.ABS_Y, dy)
                        ui_mouse.write(ecodes.EV_KEY, ecodes.BTN_LEFT, 1)
                        ui_mouse.syn()

except (OSError, KeyboardInterrupt):
    pass
finally:
    # Release all held keys
    for slot in slots.values():
        if slot.get("key") is not None:
            ui_kbd.write(ecodes.EV_KEY, slot["key"], 0)
    if st_key is not None:
        ui_kbd.write(ecodes.EV_KEY, st_key, 0)
    ui_kbd.syn()
    ui_mouse.write(ecodes.EV_KEY, ecodes.BTN_LEFT, 0)
    ui_mouse.syn()

    try:
        touch.ungrab()
    except OSError:
        pass

    ui_kbd.close()
    ui_mouse.close()
    print("\nOverlay stopped.")
