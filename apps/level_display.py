#!/usr/bin/env python3
"""
level_display.py - Python prototype for an IMU-based level display.

Reads accelerometer data from a BMI160 via SPI, computes roll and pitch,
and renders an artificial-horizon style display on the Linux framebuffer
(/dev/fb0).

Usage:
    python3 level_display.py [--csv logfile.csv]

Requirements:
    pip install spidev numpy opencv-python-headless
"""

import argparse
import math
import struct
import sys
import time

import cv2
import numpy as np

try:
    import spidev
except ImportError:
    print("ERROR: spidev module not found. Install with: pip install spidev",
          file=sys.stderr)
    sys.exit(1)


# ---------- BMI160 SPI helpers ----------

def spi_open(bus=0, device=0, speed_hz=1_000_000):
    """Open SPI bus for BMI160 communication."""
    spi = spidev.SpiDev()
    spi.open(bus, device)
    spi.max_speed_hz = speed_hz
    spi.mode = 0b00  # CPOL=0, CPHA=0
    return spi


def read_accel_raw(spi):
    """
    Read 6 bytes of accelerometer data starting at register 0x12.
    SPI read: set bit 7 of register address.
    Returns (ax, ay, az) as signed 16-bit integers.
    """
    tx = [0x80 | 0x12] + [0x00] * 6
    rx = spi.xfer2(tx)
    # rx[0] is dummy; data starts at rx[1]
    ax = struct.unpack_from('<h', bytes(rx), 1)[0]
    ay = struct.unpack_from('<h', bytes(rx), 3)[0]
    az = struct.unpack_from('<h', bytes(rx), 5)[0]
    return ax, ay, az


# ---------- orientation math ----------

ACC_SCALE = 16384.0  # LSB/g for +/-2g range


def accel_to_angles(ax, ay, az):
    """Compute roll and pitch (degrees) from raw accelerometer values."""
    gx = ax / ACC_SCALE
    gy = ay / ACC_SCALE
    gz = az / ACC_SCALE

    roll = math.atan2(gy, gz)
    pitch = math.atan2(-gx, math.sqrt(gy * gy + gz * gz))

    return math.degrees(roll), math.degrees(pitch)


# ---------- low-pass filter ----------

class LowPassFilter:
    """Simple exponential low-pass filter."""

    def __init__(self, alpha=0.1):
        self.alpha = alpha
        self.value = None

    def update(self, raw):
        if self.value is None:
            self.value = raw
        else:
            self.value = (1.0 - self.alpha) * self.value + self.alpha * raw
        return self.value


# ---------- rendering ----------

WIDTH = 640
HEIGHT = 480
SKY_COLOR = (200, 150, 50)      # BGR: blue sky
GROUND_COLOR = (50, 100, 180)   # BGR: brown ground
HORIZON_COLOR = (255, 255, 255) # white


def render_frame(roll_deg, pitch_deg):
    """
    Render an artificial-horizon frame.
    - Top half: sky (blue)
    - Bottom half: ground (brown)
    - Horizon line rotated by roll, shifted vertically by pitch.
    """
    frame = np.zeros((HEIGHT, WIDTH, 3), dtype=np.uint8)

    # Pitch shifts the horizon vertically (positive pitch = nose up = horizon
    # moves down). Scale: ~3 pixels per degree.
    horizon_y = HEIGHT // 2 + int(pitch_deg * 3.0)

    # Fill sky and ground
    if 0 < horizon_y < HEIGHT:
        frame[:horizon_y, :] = SKY_COLOR
        frame[horizon_y:, :] = GROUND_COLOR
    elif horizon_y <= 0:
        frame[:, :] = GROUND_COLOR
    else:
        frame[:, :] = SKY_COLOR

    # Draw rotated horizon line
    roll_rad = math.radians(roll_deg)
    cx, cy = WIDTH // 2, horizon_y
    half_len = int(WIDTH * 0.7)
    dx = int(half_len * math.cos(roll_rad))
    dy = int(half_len * math.sin(roll_rad))

    pt1 = (cx - dx, cy + dy)
    pt2 = (cx + dx, cy - dy)
    cv2.line(frame, pt1, pt2, HORIZON_COLOR, 2, cv2.LINE_AA)

    # Draw center crosshair
    cv2.circle(frame, (WIDTH // 2, HEIGHT // 2), 6, HORIZON_COLOR, 1,
               cv2.LINE_AA)
    cv2.line(frame, (WIDTH // 2 - 15, HEIGHT // 2),
             (WIDTH // 2 + 15, HEIGHT // 2), HORIZON_COLOR, 1, cv2.LINE_AA)
    cv2.line(frame, (WIDTH // 2, HEIGHT // 2 - 15),
             (WIDTH // 2, HEIGHT // 2 + 15), HORIZON_COLOR, 1, cv2.LINE_AA)

    # Draw text overlay
    cv2.putText(frame, f"Roll: {roll_deg:+6.1f} deg", (10, 30),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, HORIZON_COLOR, 1)
    cv2.putText(frame, f"Pitch: {pitch_deg:+6.1f} deg", (10, 55),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, HORIZON_COLOR, 1)

    return frame


def bgr_to_rgb565(frame):
    """Convert BGR888 frame to RGB565 for framebuffer output."""
    b = frame[:, :, 0].astype(np.uint16)
    g = frame[:, :, 1].astype(np.uint16)
    r = frame[:, :, 2].astype(np.uint16)
    rgb565 = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    return rgb565.astype(np.uint16).tobytes()


def write_framebuffer(data, fb_path="/dev/fb0"):
    """Write raw pixel data to the Linux framebuffer."""
    with open(fb_path, "wb") as fb:
        fb.write(data)


# ---------- main loop ----------

def main():
    parser = argparse.ArgumentParser(
        description="IMU level display on Linux framebuffer")
    parser.add_argument("--csv", type=str, default=None,
                        help="Path to CSV log file for jitter analysis")
    parser.add_argument("--spi-bus", type=int, default=0,
                        help="SPI bus number (default: 0)")
    parser.add_argument("--spi-dev", type=int, default=0,
                        help="SPI device/CS number (default: 0)")
    parser.add_argument("--spi-speed", type=int, default=1_000_000,
                        help="SPI clock speed in Hz (default: 1000000)")
    args = parser.parse_args()

    spi = spi_open(args.spi_bus, args.spi_dev, args.spi_speed)

    roll_filter = LowPassFilter(alpha=0.1)
    pitch_filter = LowPassFilter(alpha=0.1)

    csv_file = None
    if args.csv:
        csv_file = open(args.csv, "w")
        csv_file.write("timestamp_ns,dt_ms,roll_deg,pitch_deg\n")

    target_dt = 1.0 / 30.0  # ~30 fps
    prev_time = time.monotonic_ns()

    print("Level display running. Press Ctrl+C to stop.")

    try:
        while True:
            loop_start = time.monotonic_ns()

            # Read sensor
            ax, ay, az = read_accel_raw(spi)
            roll_raw, pitch_raw = accel_to_angles(ax, ay, az)

            # Filter
            roll = roll_filter.update(roll_raw)
            pitch = pitch_filter.update(pitch_raw)

            # Render
            frame = render_frame(roll, pitch)
            fb_data = bgr_to_rgb565(frame)
            write_framebuffer(fb_data)

            # Timing
            now = time.monotonic_ns()
            dt_ns = now - prev_time
            dt_ms = dt_ns / 1_000_000.0
            prev_time = now

            # Log
            if csv_file:
                csv_file.write(f"{now},{dt_ms:.3f},{roll:.2f},{pitch:.2f}\n")

            # Rate limiting
            elapsed = (time.monotonic_ns() - loop_start) / 1_000_000_000.0
            sleep_time = target_dt - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

    except KeyboardInterrupt:
        print("\nShutting down.")
    finally:
        spi.close()
        if csv_file:
            csv_file.close()
            print(f"Log saved to {args.csv}")


if __name__ == "__main__":
    main()
