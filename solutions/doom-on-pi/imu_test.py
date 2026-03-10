#!/usr/bin/env python3
"""
IMU test with visual gauge — shows pitch tilt and yaw rotation.
Calibrates neutral position at startup (hold in your comfortable
playing position — can be tilted forward ~20-30°).

Controls (type in terminal):
  c = recalibrate    f = flip pitch direction
  +/- = pitch DZ     </> = yaw DZ       q = quit
"""
import time, math, sys, select, tty, termios, glob

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
    exit(1)
print(f"Found BMI160 at {IIO}")

# ── Axis remapping — paste output from imu_axis_finder.py ──
ACCEL_FWD_AXIS   = "z"
ACCEL_FWD_SIGN   = -1
ACCEL_UP_AXIS    = "y"
GYRO_YAW_AXIS    = "y"
GYRO_YAW_SIGN    = -1

DEADZONE_PITCH = 10.0
MAX_PITCH      = 35.0
DEADZONE_YAW   = 15.0
CALIBRATION_SAMPLES = 30

accel_scale = float(open(f"{IIO}/in_accel_scale").read())
gyro_scale  = float(open(f"{IIO}/in_anglvel_scale").read())

def read_iio(prefix, axis):
    return int(open(f"{IIO}/in_{prefix}_{axis}_raw").read())

def calibrate():
    global pitch_offset
    print("\nCalibrating — hold in your comfortable playing position...")
    pitch_sum = 0.0
    for _ in range(CALIBRATION_SAMPLES):
        a_fwd = read_iio("accel", ACCEL_FWD_AXIS) * accel_scale * ACCEL_FWD_SIGN
        a_up  = read_iio("accel", ACCEL_UP_AXIS)  * accel_scale
        pitch_sum += math.atan2(a_fwd, a_up) * 180 / math.pi
        time.sleep(0.02)
    pitch_offset = pitch_sum / CALIBRATION_SAMPLES
    print(f"Neutral set at {pitch_offset:.1f}° from vertical")
    print(f"DZ: pitch=±{DEADZONE_PITCH:.0f}°  yaw=±{DEADZONE_YAW:.0f}°/s")

pitch_offset = 0.0
calibrate()

def pitch_gauge(pitch, deadzone, max_p):
    width = 35
    mid = width // 2
    dz = int(deadzone / max_p * mid)
    clamped = max(-max_p, min(max_p, pitch))
    pos = int(clamped / max_p * mid)
    bar = list("·" * width)
    for i in range(mid - dz, mid + dz + 1):
        if 0 <= i < width:
            bar[i] = "─"
    bar[mid] = "│"
    cursor_pos = mid + pos
    if 0 <= cursor_pos < width:
        bar[cursor_pos] = "█"
    return "BACK " + "".join(bar) + " FWD"

def yaw_gauge(rate, deadzone):
    width = 25
    mid = width // 2
    bar = list("·" * width)
    bar[mid] = "│"
    clamped = max(-200, min(200, rate))
    pos = int(clamped / 200 * mid)
    cursor_pos = mid + pos
    if 0 <= cursor_pos < width:
        bar[cursor_pos] = "█"
    return "L " + "".join(bar) + " R"

print("\nControls: c=recalibrate  f=flip  +/-=pitch DZ  </>=yaw DZ  q=quit\n")

old_settings = termios.tcgetattr(sys.stdin)
tty.setcbreak(sys.stdin.fileno())

try:
    while True:
        try:
            a_fwd = read_iio("accel", ACCEL_FWD_AXIS) * accel_scale * ACCEL_FWD_SIGN
            a_up  = read_iio("accel", ACCEL_UP_AXIS)  * accel_scale
            g_yaw = read_iio("anglvel", GYRO_YAW_AXIS) * gyro_scale * GYRO_YAW_SIGN
        except (FileNotFoundError, ValueError) as e:
            print(f"Read error: {e}")
            time.sleep(1)
            continue

        pitch = math.atan2(a_fwd, a_up) * 180 / math.pi - pitch_offset
        yaw_rate = g_yaw * 180 / math.pi

        action = ""
        if   pitch >  DEADZONE_PITCH: action = "FWD"
        elif pitch < -DEADZONE_PITCH: action = "BACK"
        else:                         action = "---"

        if   yaw_rate < -DEADZONE_YAW: action += " +LEFT"
        elif yaw_rate >  DEADZONE_YAW: action += " +RIGHT"

        pg = pitch_gauge(pitch, DEADZONE_PITCH, MAX_PITCH)
        yg = yaw_gauge(yaw_rate, DEADZONE_YAW)

        print(f"  Pitch:{pitch:>+6.1f}°  {pg}  |  Yaw:{yaw_rate:>+6.0f}°/s {yg}  [{action}]   ",
              end="\r")

        if select.select([sys.stdin], [], [], 0)[0]:
            ch = sys.stdin.read(1)
            if ch == 'q': break
            elif ch == 'c': calibrate()
            elif ch == 'f':
                ACCEL_FWD_SIGN *= -1
                print(f"\nFlipped pitch direction")
                calibrate()
            elif ch in ('+', '='):
                DEADZONE_PITCH = min(45, DEADZONE_PITCH + 2)
                print(f"\nPitch DZ: ±{DEADZONE_PITCH:.0f}°")
            elif ch == '-':
                DEADZONE_PITCH = max(3, DEADZONE_PITCH - 2)
                print(f"\nPitch DZ: ±{DEADZONE_PITCH:.0f}°")
            elif ch in ('.', '>'):
                DEADZONE_YAW = min(60, DEADZONE_YAW + 2)
                print(f"\nYaw DZ: ±{DEADZONE_YAW:.0f}°/s")
            elif ch in (',', '<'):
                DEADZONE_YAW = max(5, DEADZONE_YAW - 2)
                print(f"\nYaw DZ: ±{DEADZONE_YAW:.0f}°/s")

        time.sleep(0.05)
except KeyboardInterrupt:
    pass
finally:
    termios.tcsetattr(sys.stdin, termios.TCSADRAIN, old_settings)
    print()
