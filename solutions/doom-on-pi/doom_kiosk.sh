#!/bin/bash
# Adjust display size for your setup:
DISP_W=800
DISP_H=480

# Kill stale processes from previous runs
pkill -f doom_touch_overlay 2>/dev/null
pkill -f imu_doom 2>/dev/null
pkill -f drm_overlay 2>/dev/null
sleep 0.5

# Load IMU driver (no-op if already loaded)
modprobe bmi160_spi 2>/dev/null

# Start DRM overlay for visible button labels (Option B)
/home/linux/drm_overlay &
DRM_PID=$!
sleep 0.5

# Start touch overlay — auto-detects touchscreen device
python3 /home/linux/doom_touch_overlay.py \
    --width $DISP_W --height $DISP_H &
OVERLAY_PID=$!

# Start IMU controller (optional — comment out if no BMI160)
python3 /home/linux/imu_doom.py --neutral 25 --pitch-dz 15 --yaw-dz 15 &
IMU_PID=$!

# Wait for virtual input devices to appear before starting Doom.
# SDL2 enumerates /dev/input/event* during init — if virtual
# keyboards don't exist yet, Doom won't see them.
sleep 2

# Start Doom (SDL2 discovers virtual keyboard + mouse during init)
/home/linux/chocolate-doom/src/chocolate-doom \
    -iwad /home/linux/.local/share/chocolate-doom/doom1.wad

# Clean up when Doom exits
kill $OVERLAY_PID $IMU_PID $DRM_PID 2>/dev/null
