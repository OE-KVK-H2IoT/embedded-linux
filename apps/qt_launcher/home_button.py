#!/usr/bin/env python3
"""
GPIO Home Button daemon for Qt App Launcher.

Monitors a physical pushbutton on a GPIO pin. When pressed, sends SIGTERM
to the currently running child application (via PID file), causing the
launcher to reclaim the display.

Wiring: pushbutton between GPIO17 (pin 11) and GND (pin 9).
        Internal pull-up is enabled — the line reads HIGH when idle,
        LOW when the button is pressed.

Usage:
    sudo python3 home_button.py [--chip CHIP] [--line LINE]

Requires: python3-gpiod (apt install python3-gpiod)
"""

import gpiod
import signal
import os
import sys
import argparse
import time

PID_FILE = "/tmp/launcher_child.pid"

# Debounce: ignore edges closer than this (seconds)
DEBOUNCE_SEC = 0.3


def kill_child():
    """Read the child PID file and send SIGTERM."""
    try:
        with open(PID_FILE) as f:
            pid = int(f.read().strip())
        os.kill(pid, signal.SIGTERM)
        print(f"[home_button] Sent SIGTERM to child PID {pid}")
    except FileNotFoundError:
        pass  # No child running
    except ValueError:
        print("[home_button] Invalid PID file contents", file=sys.stderr)
    except ProcessLookupError:
        # Process already gone — clean up stale PID file
        try:
            os.remove(PID_FILE)
        except OSError:
            pass


def main():
    parser = argparse.ArgumentParser(description="GPIO Home Button daemon")
    parser.add_argument("--chip", default="/dev/gpiochip4",
                        help="GPIO chip device (Pi 5: /dev/gpiochip4, Pi 4: /dev/gpiochip0)")
    parser.add_argument("--line", type=int, default=17,
                        help="GPIO line number (default: 17)")
    args = parser.parse_args()

    print(f"[home_button] Monitoring {args.chip} line {args.line} "
          f"(falling edge, pull-up enabled)")

    request = gpiod.request_lines(
        args.chip,
        consumer="home-button",
        config={
            args.line: gpiod.LineSettings(
                direction=gpiod.line.Direction.INPUT,
                bias=gpiod.line.Bias.PULL_UP,
                edge_detection=gpiod.line.Edge.FALLING,
            )
        },
    )

    last_press = 0.0

    try:
        while True:
            # Wait for edge events (blocking)
            if request.wait_edge_events(timeout=None):
                events = request.read_edge_events()
                now = time.monotonic()
                for _ in events:
                    if now - last_press >= DEBOUNCE_SEC:
                        last_press = now
                        kill_child()
    except KeyboardInterrupt:
        print("\n[home_button] Stopped")
    finally:
        request.release()


if __name__ == "__main__":
    main()
