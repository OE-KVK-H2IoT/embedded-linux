#!/usr/bin/env python3
"""
Map BMI160 motion to Doom arrow keys via uinput.

Pitch (tilt forward/backward) → KEY_UP / KEY_DOWN (walk)
  Uses gravity vector (accelerometer) to measure absolute tilt angle.
  Key is HELD while tilted past the deadzone. Return to neutral = stop.

Yaw (rotate left/right) → KEY_LEFT / KEY_RIGHT (turn)
  Uses gyro angular velocity directly (rate-based, no drift).

Usage:
  sudo python3 imu_doom.py [--neutral DEGREES] [--pitch-dz DEGREES] [--yaw-dz DEG/S] [--flip]

  --neutral   Neutral tilt angle from vertical (default: 25, comfortable lean)
  --pitch-dz  Pitch deadzone in degrees (default: 15)
  --yaw-dz    Yaw deadzone in degrees/sec (default: 15)
  --flip      Invert pitch direction (if forward/backward are swapped)
"""
import time, math, argparse, sys, os, glob
import evdev
from evdev import UInput, ecodes

# ── Command line arguments ──────────────────────────────────
parser = argparse.ArgumentParser(description="IMU Doom controller")
parser.add_argument("--neutral", type=float, default=25.0,
                    help="Neutral tilt angle from vertical in degrees (default: 25)")
parser.add_argument("--pitch-dz", type=float, default=15.0,
                    help="Pitch deadzone in degrees (default: 15)")
parser.add_argument("--yaw-dz", type=float, default=15.0,
                    help="Yaw deadzone in deg/s (default: 15)")
parser.add_argument("--flip", action="store_true",
                    help="Invert pitch direction")
args = parser.parse_args()

# ── Auto-detect IIO device ──────────────────────────────────
def find_bmi160():
    for dev in sorted(glob.glob("/sys/bus/iio/devices/iio:device*")):
        try:
            name = open(f"{dev}/name").read().strip()
            if "bmi160" in name:
                return dev
        except FileNotFoundError:
            continue
    return None

IIO = find_bmi160()
if not IIO:
    # Try loading the driver automatically
    os.system("modprobe bmi160_spi 2>/dev/null")
    time.sleep(0.5)
    IIO = find_bmi160()
if not IIO:
    print("ERROR: BMI160 not found. Check wiring and driver.")
    exit(1)
print(f"Found BMI160 at {IIO}")

# ── IMU configuration ────────────────────────────────────────
pitch_offset   = args.neutral
deadzone_pitch = args.pitch_dz
deadzone_yaw   = args.yaw_dz
POLL_INTERVAL  = 0.01    # 100 Hz

# ── Axis remapping — paste output from imu_axis_finder.py ──
ACCEL_FWD_AXIS   = "z"
ACCEL_FWD_SIGN   = (1 if args.flip else -1)
ACCEL_UP_AXIS    = "y"
GYRO_YAW_AXIS    = "y"
GYRO_YAW_SIGN    = -1

accel_scale = float(open(f"{IIO}/in_accel_scale").read())
gyro_scale = float(open(f"{IIO}/in_anglvel_scale").read())

def read_iio(prefix, axis):
    return int(open(f"{IIO}/in_{prefix}_{axis}_raw").read())

# ── Virtual keyboard ────────────────────────────────────────
KEYS = [ecodes.KEY_UP, ecodes.KEY_DOWN, ecodes.KEY_LEFT, ecodes.KEY_RIGHT]
ui = UInput({ecodes.EV_KEY: KEYS}, name="doom-imu-controller")
print(f"IMU keyboard: {ui.device.path}")

held = {k: False for k in KEYS}

def set_key(key, pressed):
    if held[key] != pressed:
        ui.write(ecodes.EV_KEY, key, 1 if pressed else 0)
        held[key] = pressed

print(f"Neutral: {pitch_offset:.0f}°  Pitch DZ: ±{deadzone_pitch:.0f}°  Yaw DZ: ±{deadzone_yaw:.0f}°/s")
print("Ready! Tilt to walk, rotate to turn.")

# ── Main loop ───────────────────────────────────────────────
iteration = 0
try:
    while True:
        # Read accelerometer for pitch angle (gravity-based)
        a_fwd = read_iio("accel", ACCEL_FWD_AXIS) * accel_scale * ACCEL_FWD_SIGN
        a_up  = read_iio("accel", ACCEL_UP_AXIS)  * accel_scale

        # Read gyro for yaw rate
        g_yaw = read_iio("anglvel", GYRO_YAW_AXIS) * gyro_scale * GYRO_YAW_SIGN

        # Pitch angle from gravity, relative to neutral lean angle
        pitch = math.atan2(a_fwd, a_up) * 180 / math.pi - pitch_offset

        # Yaw rate in °/s
        yaw_rate = g_yaw * 180 / math.pi

        # Pitch → hold key while tilted past deadzone
        set_key(ecodes.KEY_UP,    pitch >  deadzone_pitch)
        set_key(ecodes.KEY_DOWN,  pitch < -deadzone_pitch)

        # Yaw → hold key while rotating past deadzone
        set_key(ecodes.KEY_LEFT,  yaw_rate < -deadzone_yaw)
        set_key(ecodes.KEY_RIGHT, yaw_rate >  deadzone_yaw)
        ui.syn()

        # Print status every ~0.5s
        iteration += 1
        if iteration % 50 == 0:
            fwd = "FWD" if pitch > deadzone_pitch else "BACK" if pitch < -deadzone_pitch else "---"
            yaw = "LEFT" if yaw_rate < -deadzone_yaw else "RIGHT" if yaw_rate > deadzone_yaw else "---"
            print(f"\r  pitch={pitch:+6.1f}° [{fwd:>4}]  yaw={yaw_rate:+6.1f}°/s [{yaw:>5}]  "
                  f"DZ: p=±{deadzone_pitch:.0f}° y=±{deadzone_yaw:.0f}°/s", end="", flush=True)

        time.sleep(POLL_INTERVAL)

except KeyboardInterrupt:
    pass
finally:
    for key in KEYS:
        if held[key]:
            ui.write(ecodes.EV_KEY, key, 0)
    ui.syn()
    ui.close()
    print(f"\nFinal settings: --neutral {pitch_offset:.0f} --pitch-dz {deadzone_pitch:.0f} --yaw-dz {deadzone_yaw:.0f}"
          + (" --flip" if ACCEL_FWD_SIGN == 1 else ""))
