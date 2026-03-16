#!/usr/bin/env python3
"""
Microphone gain test with live waveform + loopback to headphones.
Use this to verify your mic picks up keystrokes clearly before
running the acoustic keystroke recognition tutorial.

Usage:
    python mic_test.py                  # use default devices
    python mic_test.py --list           # list audio devices
    python mic_test.py -i 4 -o 15      # specify input/output device
    python mic_test.py --gain 3.0       # boost mic gain for loopback
"""

import argparse
import sys
import threading
import time
import numpy as np
import sounddevice as sd

RATE = 48000
BLOCK = 256  # ~5ms blocks for low latency
HISTORY_SEC = 3

TERM_WIDTH = 80
TERM_HEIGHT = 20


def list_devices():
    print(sd.query_devices())
    sys.exit(0)


def draw_waveform(buf, peak, rms):
    """Draw a live ASCII waveform + level meter in the terminal."""
    rows = TERM_HEIGHT
    cols = TERM_WIDTH - 2

    n = len(buf)
    step = max(1, n // cols)
    samples = buf[::step][:cols]

    scale = max(peak * 1.2, 0.01)

    lines = []

    # Level meter
    bar_len = cols - 12
    rms_pos = int(min(rms / scale, 1.0) * bar_len)
    peak_pos = int(min(peak / scale, 1.0) * bar_len)
    meter = list('─' * bar_len)
    for i in range(rms_pos):
        meter[i] = '█' if i < bar_len * 0.6 else ('▓' if i < bar_len * 0.85 else '░')
    if peak_pos < bar_len:
        meter[peak_pos] = '|'
    rms_db = 20 * np.log10(rms + 1e-10)
    peak_db = 20 * np.log10(peak + 1e-10)
    lines.append(f"  RMS:{rms_db:+5.0f}dB [{''.join(meter)}] Pk:{peak_db:+.0f}dB")
    lines.append("")

    # Waveform
    for r in range(rows):
        y_top = 1.0 - (r / rows) * 2
        y_bot = 1.0 - ((r + 1) / rows) * 2

        row_chars = []
        for s in samples:
            val = s / scale
            if y_bot <= 0 <= y_top and abs(val) < abs(y_bot):
                row_chars.append('─')
            elif min(val, 0) <= y_top and max(val, 0) >= y_bot:
                row_chars.append('█')
            else:
                row_chars.append(' ')
        lines.append('│' + ''.join(row_chars) + '│')

    lines.append("")
    lines.append("  Press Ctrl+C to stop")

    output = f"\033[{len(lines)}A" + '\n'.join(lines)
    sys.stdout.write(output + '\n')
    sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser(description="Mic test: loopback + live waveform")
    parser.add_argument('--list', action='store_true', help='List audio devices')
    parser.add_argument('-i', '--input', type=int, default=None, help='Input device index')
    parser.add_argument('-o', '--output', type=int, default=None, help='Output device index')
    parser.add_argument('--gain', type=float, default=1.0, help='Mic gain multiplier for loopback')
    parser.add_argument('--no-loopback', action='store_true', help='Disable audio loopback to output')
    parser.add_argument('--latency', type=str, default='low', help='Latency setting: low, high, or ms value')
    args = parser.parse_args()

    if args.list:
        list_devices()

    # Parse latency
    try:
        latency = float(args.latency) / 1000  # ms to seconds
    except ValueError:
        latency = args.latency  # 'low' or 'high'

    print(f"Mic Test — rate={RATE} block={BLOCK} gain={args.gain}x latency={args.latency}")
    print(f"Input: {args.input or 'default'}  Output: {args.output or 'default'}")
    print(f"Loopback: {'OFF' if args.no_loopback else 'ON'}")
    print("Type on the keyboard — watch the waveform and listen in your headphones.")
    print()

    for _ in range(TERM_HEIGHT + 5):
        print()

    buf_len = RATE * HISTORY_SEC
    waveform_buf = np.zeros(buf_len, dtype=np.float32)
    state = {'pos': 0, 'peak': 0.0, 'rms': 0.0}
    lock = threading.Lock()

    do_loopback = not args.no_loopback
    gain = args.gain

    def callback(indata, outdata, frames, time_info, status):
        audio = indata[:, 0]

        # Loopback — direct copy, no processing delay
        if do_loopback:
            outdata[:, :] = (audio * gain).reshape(-1, 1)
        else:
            outdata.fill(0)

        # Update ring buffer (lock-free is fine for single writer)
        pos = state['pos']
        n = len(audio)
        end = pos + n
        if end <= buf_len:
            waveform_buf[pos:end] = audio
        else:
            wrap = end - buf_len
            waveform_buf[pos:] = audio[:n - wrap]
            waveform_buf[:wrap] = audio[n - wrap:]
        state['pos'] = end % buf_len

        # Stats
        rms = np.sqrt(np.mean(audio ** 2))
        cur_peak = np.max(np.abs(audio))
        state['rms'] = rms
        state['peak'] = max(cur_peak, state['peak'] * 0.995)

    # Draw in a separate thread at ~30fps, not in the audio callback
    running = threading.Event()
    running.set()

    def draw_loop():
        while running.is_set():
            pos = state['pos']
            snapshot = np.roll(waveform_buf.copy(), -pos)
            draw_waveform(snapshot, state['peak'], state['rms'])
            time.sleep(0.033)  # ~30 fps

    draw_thread = threading.Thread(target=draw_loop, daemon=True)

    try:
        with sd.Stream(
            samplerate=RATE,
            blocksize=BLOCK,
            device=(args.input, args.output),
            channels=(1, 2),
            dtype='float32',
            latency=latency,
            callback=callback
        ):
            draw_thread.start()
            print("Stream started. Press Ctrl+C to stop.\n")
            while True:
                time.sleep(0.1)
    except KeyboardInterrupt:
        running.clear()
        print("\nStopped.")
    except sd.PortAudioError as e:
        running.clear()
        print(f"\nAudio error: {e}")
        print("Try specifying devices with --list and -i/-o flags")
        sys.exit(1)


if __name__ == "__main__":
    main()
