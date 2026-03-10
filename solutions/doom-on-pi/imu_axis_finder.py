#!/usr/bin/env python3
"""Gravity-based axis discovery for the BMI160 IMU.

Place the display in two positions. Gravity does the rest.
"""
import time, sys, math, glob

# Auto-detect IIO device (number can change between reboots)
IIO = None
for dev in sorted(glob.glob("/sys/bus/iio/devices/iio:device*")):
    try:
        name = open(f"{dev}/name").read().strip()
        if "bmi160" in name:
            IIO = dev
            break
    except FileNotFoundError:
        continue
if not IIO:
    print("ERROR: BMI160 not found. Load the driver: sudo modprobe bmi160_spi")
    sys.exit(1)
print(f"Found BMI160 at {IIO}")

accel_scale = float(open(f"{IIO}/in_accel_scale").read())
gyro_scale  = float(open(f"{IIO}/in_anglvel_scale").read())

AXES = ["x", "y", "z"]

def read_accel():
    return {a: int(open(f"{IIO}/in_accel_{a}_raw").read()) * accel_scale
            for a in AXES}

def read_gyro():
    return {a: int(open(f"{IIO}/in_anglvel_{a}_raw").read()) * gyro_scale
            for a in AXES}

def sample(read_fn, seconds=2):
    """Average readings over time."""
    sums = {a: 0.0 for a in AXES}
    n = 0
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        vals = read_fn()
        for a in AXES:
            sums[a] += vals[a]
        n += 1
        time.sleep(0.02)
    return {a: sums[a] / n for a in AXES}

def show_accel(label, vals):
    print(f"\n    {label}:")
    for a in AXES:
        bar = "█" * int(abs(vals[a]))
        print(f"      {a}: {vals[a]:>8.2f} m/s²  {bar}")

print("=" * 55)
print("  IMU Axis Finder — gravity-based auto-detection")
print("=" * 55)
print()
print("  The IMU is attached to the display, so placing the")
print("  display in known positions tells us the axis mapping.")

# ── Position 1: Flat on table ──
input("\n>>> Step 1: Place the display FLAT on a table (screen up).\n"
      "    Press ENTER when ready...")
print("    Sampling for 2s — hold still...")
flat = sample(read_accel)
show_accel("Flat position (gravity points through screen)", flat)

# ── Position 2: Upright in playing position ──
input("\n>>> Step 2: Hold the display UPRIGHT in your playing position\n"
      "    (screen facing you, as if playing Doom).\n"
      "    Press ENTER when ready...")
print("    Sampling for 2s — hold still...")
upright = sample(read_accel)
show_accel("Upright/playing position (gravity points down)", upright)

# ── Detect yaw gyro axis ──
input("\n>>> Step 3: ROTATE LEFT (turn your body) and keep rotating\n"
      "    slowly until sampling finishes.\n"
      "    Press ENTER, then start rotating...")
print("    Sampling gyro for 3s — ROTATE NOW...")
peak_gyro = {a: 0.0 for a in AXES}
end = time.monotonic() + 3
while time.monotonic() < end:
    vals = read_gyro()
    for a in AXES:
        if abs(vals[a]) > peak_gyro[a]:
            peak_gyro[a] = abs(vals[a])
    time.sleep(0.02)
# Also sample a short "held left" for sign detection
left_gyro = sample(read_gyro, seconds=1)

# ── Determine axis mapping from gravity ──
print("\n" + "=" * 55)
print("  ANALYSIS")
print("=" * 55)

# UP axis: strongest gravity reading when held upright
up_axis = max(AXES, key=lambda a: abs(upright[a]))
up_sign = 1 if upright[up_axis] > 0 else -1
print(f"\n  UP axis (gravity when upright): {up_axis} = {upright[up_axis]:+.2f} m/s²")

# FWD axis: strongest gravity reading when flat, excluding UP axis
# When flat, gravity points through the screen. When you tilt forward
# from upright, gravity component shifts toward this axis.
remaining = [a for a in AXES if a != up_axis]
fwd_axis = max(remaining, key=lambda a: abs(flat[a]))
# Sign: we want positive a_fwd when tilting forward (top away).
# Tilting forward shifts the FWD axis toward the flat reading.
# If flat reads negative, forward tilt goes negative, so sign = -1.
fwd_sign = -1 if flat[fwd_axis] < 0 else 1
print(f"  FWD axis (gravity when flat):   {fwd_axis} = {flat[fwd_axis]:+.2f} m/s²")

# SIDE axis: the remaining one
side_axis = [a for a in AXES if a not in (up_axis, fwd_axis)][0]
print(f"  SIDE axis (left-right):         {side_axis}")

# YAW gyro axis: strongest gyro during rotation
yaw_axis = max(AXES, key=lambda a: peak_gyro[a])
yaw_sign = -1 if left_gyro[yaw_axis] > 0 else 1
print(f"  YAW gyro axis:                  {yaw_axis} (peak: {peak_gyro[yaw_axis]:.4f})")

# Sanity checks
print("\n  Sanity checks:")
if up_axis == fwd_axis:
    print("  ⚠ WARNING: UP and FWD resolved to the same axis!")
    print("    Make sure position 1 was truly flat and position 2 truly upright.")
else:
    print(f"  ✓ UP ({up_axis}) and FWD ({fwd_axis}) are different axes")

# Check that upright UP reading is near 9.8
if abs(upright[up_axis]) < 7.0:
    print(f"  ⚠ WARNING: UP axis reads only {upright[up_axis]:.1f} m/s²")
    print("    Expected ~9.8 m/s². Was the display truly upright?")
else:
    print(f"  ✓ UP axis reads {abs(upright[up_axis]):.1f} m/s² (expected ~9.8)")

# Check that flat FWD reading is near 9.8
if abs(flat[fwd_axis]) < 7.0:
    print(f"  ⚠ WARNING: FWD axis reads only {flat[fwd_axis]:.1f} m/s² when flat")
    print("    Expected ~9.8 m/s². Was the display truly flat?")
else:
    print(f"  ✓ FWD axis reads {abs(flat[fwd_axis]):.1f} m/s² when flat (expected ~9.8)")

# ── Output ──
print("\n" + "=" * 55)
print("  RESULT — copy these into imu_doom.py:")
print("=" * 55)
print(f'''
ACCEL_FWD_AXIS   = "{fwd_axis}"
ACCEL_FWD_SIGN   = {fwd_sign}
ACCEL_UP_AXIS    = "{up_axis}"
GYRO_YAW_AXIS    = "{yaw_axis}"
GYRO_YAW_SIGN    = {yaw_sign}
''')
print("Then run:  sudo python3 ~/imu_doom.py")
print("If forward/backward is inverted, press 'f' to flip.")
