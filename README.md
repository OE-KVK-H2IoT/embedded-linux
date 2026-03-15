# Embedded Linux

Source code and labs for the Linux in Embedded Systems course at Obuda University.

## Structure

- `apps/` — Graphical demo applications (one subfolder per tutorial)
- `drivers/` — Linux kernel modules and device drivers
- `overlays/` — Device tree overlays
- `scripts/` — Tutorial scripts (one subfolder per tutorial)
- `services/` — Systemd service files
- `solutions/` — Lab exercise solutions

## Apps

| Folder | Description |
|--------|-------------|
| `apps/i2s-audio-viz/` | I2S microphone spectrum analyzer + direction detection |
| `apps/level-display/` | IMU attitude indicator (Python + SDL2 versions) |
| `apps/pong-fb/` | Framebuffer Pong game |
| `apps/sdl2-dashboard/` | Multi-panel sensor dashboard |
| `apps/iio-buffered-capture/` | High-rate IIO sensor capture |
| `apps/processes-and-ipc/` | Thread-safe sensor examples |
| `apps/qt_dashboard/` | Qt6 QML dashboard |
| `apps/qt_launcher/` | Qt6 multi-app launcher |

## Scripts

| Folder | Description |
|--------|-------------|
| `scripts/acoustic-keystroke/` | Acoustic keystroke recognition pipeline |
| `scripts/jitter-measurement/` | Timing analysis tools |
| `scripts/kiosk-service/` | Kiosk app switcher |
| `scripts/ssh-login/` | Pi first-boot setup |

## Course Documentation

Full course documentation: https://www.aut.uni-obuda.hu/es/
