#!/usr/bin/env bash
# setup_pi.sh — Prepare a Raspberry Pi OS (Trixie) SD card for Embedded Linux labs
#
# Writes first-boot configuration to the boot partition:
#   - User account, WiFi, SSH, hostname
#   - Hardware interfaces (I2C, SPI, GPU)
#   - firstrun.sh script that installs lab packages on first boot
#
# Tested with: 2025-12-04-raspios-trixie-arm64-lite.img.xz
#
# Usage:
#   ./setup_pi.sh                          # auto-detect boot partition
#   ./setup_pi.sh /run/media/user/bootfs   # explicit path (Arch)
#   ./setup_pi.sh /media/user/bootfs       # explicit path (Ubuntu)
#
# Copyright (C) 2025 Obuda University — Embedded Systems Lab
# SPDX-License-Identifier: MIT
set -euo pipefail

# ── Configuration ─────────────────────────────────────────────
HOSTNAME="eslinux"
USERNAME="linux"
USER_PASSWORD="passwd"
WIFI_COUNTRY="HU"
WIFI_SSID="C005"
WIFI_PASSWORD="C005passwd"

# ── Find boot partition ───────────────────────────────────────

find_bootfs() {
    local candidates=(
        /run/media/"$USER"/bootfs               # Arch / Fedora
        /run/media/"$USER"/BOOTFS
        /media/"$USER"/bootfs                   # Ubuntu / Debian
        /media/"$USER"/BOOTFS
        /mnt/bootfs                             # manual mount
    )

    for p in "${candidates[@]}"; do
        if [ -d "$p" ] && [ -f "$p/config.txt" ]; then
            echo "$p"
            return 0
        fi
    done
    return 1
}

if [ $# -ge 1 ]; then
    BOOT_PATH="$1"
    if [ ! -d "$BOOT_PATH" ]; then
        echo "Error: '$BOOT_PATH' is not a directory" >&2
        exit 1
    fi
else
    BOOT_PATH="$(find_bootfs)" || {
        echo "Error: could not find boot partition." >&2
        echo "Mount the SD card and pass the bootfs path:" >&2
        echo "  $0 /path/to/bootfs" >&2
        exit 1
    }
    echo "Auto-detected boot partition: $BOOT_PATH"
fi

# Verify it looks like a Pi boot partition
if [ ! -f "$BOOT_PATH/config.txt" ]; then
    echo "Warning: no config.txt found in $BOOT_PATH" >&2
    read -rp "Continue anyway? [y/N] " ans
    [[ "$ans" =~ ^[Yy] ]] || exit 1
fi

# ── Generate password hash ────────────────────────────────────
HASH="$(printf '%s' "$USER_PASSWORD" | openssl passwd -6 -stdin)"

# ── RPi OS user configuration ────────────────────────────────
# userconf.txt: username:password_hash — RPi OS creates this user on first boot
echo "${USERNAME}:${HASH}" > "$BOOT_PATH/userconf.txt"

# ── Enable SSH ────────────────────────────────────────────────
touch "$BOOT_PATH/ssh"

# ── WiFi configuration ────────────────────────────────────────
# RPi OS Trixie uses NetworkManager; drop a connection file for first boot
cat > "$BOOT_PATH/wpa_supplicant.conf" <<EOF
country=$WIFI_COUNTRY
ctrl_interface=DIR=/var/run/wpa_supplicant GROUP=netdev
update_config=1

network={
    ssid="$WIFI_SSID"
    psk="$WIFI_PASSWORD"
    key_mgmt=WPA-PSK
}
EOF

# ── Enable hardware interfaces in config.txt ──────────────────
CONFIG="$BOOT_PATH/config.txt"

# Add a line to config.txt if not already present
add_config() {
    if ! grep -qxF "$1" "$CONFIG" 2>/dev/null; then
        echo "$1" >> "$CONFIG"
    fi
}

echo "" >> "$CONFIG"
echo "# --- Embedded Linux Lab Configuration ---" >> "$CONFIG"

# I2C (MCP9808, SSD1306 OLED)
add_config "dtparam=i2c_arm=on"

# SPI (BMI160 IMU, BUSE LED matrix, SPI displays)
add_config "dtparam=spi=on"

# GPU driver for DRM/KMS, SDL2, Qt EGLFS
add_config "dtoverlay=vc4-kms-v3d"

# GPU memory for graphics-heavy labs
add_config "gpu_mem=128"

# ── First-run script ──────────────────────────────────────────
# RPi OS executes /boot/firmware/firstrun.sh on first boot, then deletes it.
cat > "$BOOT_PATH/firstrun.sh" <<'FIRSTRUN'
#!/bin/bash
set -e

# ── Hostname ──────────────────────────────────────────────────
HOSTNAME="eslinux"
USERNAME="linux"

raspi-config nonint do_hostname "$HOSTNAME"

# ── Add user to hardware groups ───────────────────────────────
for grp in i2c spi gpio video render input dialout plugdev netdev; do
    getent group "$grp" >/dev/null 2>&1 || groupadd "$grp"
    usermod -aG "$grp" "$USERNAME" 2>/dev/null || true
done

# ── WiFi country ──────────────────────────────────────────────
raspi-config nonint do_wifi_country "HU"

# ── udev rules for non-root I2C/SPI access ────────────────────
cat > /etc/udev/rules.d/99-i2c-spi.rules <<'UDEV'
SUBSYSTEM=="i2c-dev", GROUP="i2c", MODE="0660"
SUBSYSTEM=="spidev", GROUP="spi", MODE="0660"
UDEV

# ── Disable fbcon cursor (prevents blinking cursor on OLED fb) ─
cat > /etc/systemd/system/fb-nocursor.service <<'FBSVC'
[Unit]
Description=Disable fbcon cursor on OLED
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'echo 0 > /sys/class/graphics/fbcon/cursor_blink; for f in /sys/class/vtconsole/vtcon*/bind; do echo 0 > "$f"; done 2>/dev/null; true'

[Install]
WantedBy=multi-user.target
FBSVC
systemctl enable fb-nocursor.service

# ── Install lab packages ──────────────────────────────────────
apt-get update

# Build tools & kernel dev
apt-get install -y \
    build-essential git cmake device-tree-compiler \
    linux-headers-arm64

# I2C / SPI / GPIO
apt-get install -y \
    i2c-tools python3-smbus2 \
    gpiod python3-gpiod

# SDL2 & graphics
apt-get install -y \
    libsdl2-dev libsdl2-ttf-dev \
    libgles2-mesa-dev libdrm-dev libdrm-tests \
    mesa-utils fonts-dejavu-core fbi

# Python libraries
apt-get install -y \
    python3-pil python3-numpy python3-evdev python3-psutil

# Camera & vision
apt-get install -y \
    python3-picamera2 python3-opencv || true

# Performance & RT
apt-get install -y \
    stress-ng rt-tests trace-cmd

# Qt6 (dashboard & launcher labs)
apt-get install -y \
    qt6-base-dev qt6-declarative-dev || true

# Networking & security labs
apt-get install -y \
    nftables mosquitto mosquitto-clients || true

# Utilities
apt-get install -y evtest wget

# ── Clone lab repository ──────────────────────────────────────
su - "$USERNAME" -c \
    'git clone https://github.com/OE-KVK-H2IoT/embedded-linux.git ~/embedded-linux' || true

# ── Clean up ──────────────────────────────────────────────────
apt-get clean
rm -f /boot/firmware/firstrun.sh
FIRSTRUN

chmod +x "$BOOT_PATH/firstrun.sh"

# ── Inject firstrun.sh into cmdline.txt ───────────────────────
# RPi OS needs "systemd.run=/boot/firmware/firstrun.sh" in the kernel cmdline
CMDLINE="$BOOT_PATH/cmdline.txt"
if [ -f "$CMDLINE" ]; then
    if ! grep -q "firstrun.sh" "$CMDLINE"; then
        # Append to the single-line cmdline
        sed -i 's/$/ systemd.run=\/boot\/firmware\/firstrun.sh systemd.run_success_action=reboot systemd.unit=kernel-command-line.target/' "$CMDLINE"
    fi
fi

# ── Summary ───────────────────────────────────────────────────
echo ""
echo "Setup complete: $BOOT_PATH"
echo ""
echo "  Files written:"
echo "    $BOOT_PATH/userconf.txt          — user '$USERNAME'"
echo "    $BOOT_PATH/ssh                   — SSH enabled"
echo "    $BOOT_PATH/wpa_supplicant.conf   — WiFi '$WIFI_SSID'"
echo "    $BOOT_PATH/firstrun.sh           — package install on first boot"
echo ""
echo "  config.txt additions:"
echo "    dtparam=i2c_arm=on"
echo "    dtparam=spi=on"
echo "    dtoverlay=vc4-kms-v3d"
echo "    gpu_mem=128"
echo ""
echo "  cmdline.txt: firstrun.sh hook added"
echo ""
echo "  First boot will (~10-15 min with internet):"
echo "    - Create user '$USERNAME' (password: '$USER_PASSWORD')"
echo "    - Set hostname to '$HOSTNAME'"
echo "    - Connect to WiFi '$WIFI_SSID'"
echo "    - Install ~30 packages for all labs"
echo "    - Clone embedded-linux repo to ~/embedded-linux"
echo "    - Reboot when done"
echo ""
echo "  After reboot:"
echo "    ssh $USERNAME@$HOSTNAME.local"
