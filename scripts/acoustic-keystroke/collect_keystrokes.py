#!/usr/bin/env python3
"""Record keystrokes with synchronized audio for training data.

Usage:
    python collect_keystrokes.py                        # default settings
    python collect_keystrokes.py --keys abcdef          # just 6 keys
    python collect_keystrokes.py --presses 30           # 30 presses per key
    python collect_keystrokes.py --gain 10              # boost mic 10x
    python collect_keystrokes.py --device 4             # device index
    python collect_keystrokes.py --list                 # list audio devices
"""

import argparse
import sys
import threading
import time
import numpy as np
import sounddevice as sd
from pathlib import Path

RATE = 48000
CHANNELS = 1
BLOCK = 512  # ~10ms


# ── High-pass filter (remove rumble below 80Hz) ─────────────────────

def make_highpass(cutoff_hz, rate):
    """Simple first-order IIR high-pass filter coefficients."""
    rc = 1.0 / (2.0 * np.pi * cutoff_hz)
    dt = 1.0 / rate
    alpha = rc / (rc + dt)
    return alpha


class HighPass:
    """Stateful first-order high-pass filter."""
    def __init__(self, cutoff_hz, rate):
        self.alpha = make_highpass(cutoff_hz, rate)
        self.prev_in = 0.0
        self.prev_out = 0.0

    def process(self, samples):
        out = np.empty_like(samples)
        a = self.alpha
        yi = self.prev_out
        xi_prev = self.prev_in
        for i in range(len(samples)):
            xi = samples[i]
            yi = a * (yi + xi - xi_prev)
            xi_prev = xi
            out[i] = yi
        self.prev_in = xi_prev
        self.prev_out = yi
        return out


# ── Level meter ──────────────────────────────────────────────────────

def level_bar(rms, peak, width=30):
    """ASCII level meter with RMS bar and peak marker."""
    # Scale to 0..1 range (assuming ~0.5 is loud after gain)
    rms_norm = min(1.0, rms * 2)
    peak_norm = min(1.0, peak * 2)

    rms_pos = int(rms_norm * width)
    peak_pos = int(peak_norm * width)

    bar = list('░' * width)
    for i in range(rms_pos):
        if i < width * 0.6:
            bar[i] = '█'
        elif i < width * 0.85:
            bar[i] = '▓'
        else:
            bar[i] = '▒'
    if peak_pos < width:
        bar[peak_pos] = '│'

    rms_db = 20 * np.log10(rms + 1e-10)
    return f"[{''.join(bar)}] {rms_db:+5.0f}dB"


# ── Key collector ────────────────────────────────────────────────────

def collect_key(label, num_presses, device, rate, gain, hp_filter, threshold):
    """Record audio for one key, stop after num_presses detected."""
    max_duration = num_presses * 2 + 5
    max_samples = int(max_duration * rate)
    audio_buf = np.zeros(max_samples, dtype=np.float32)
    state = {
        'pos': 0,
        'count': 0,
        'energy_avg': 0.0,
        'cooldown': 0,
        'done': threading.Event(),
        'peak': 0.0,
        'rms': 0.0,
        'onset_energies': [],  # track SNR of each detection
    }

    def callback(indata, frames, time_info, status):
        chunk = indata[:, 0].copy()

        # Apply gain
        chunk *= gain

        # Apply high-pass filter (removes rumble, improves SNR)
        chunk = hp_filter.process(chunk)

        pos = state['pos']
        n = len(chunk)

        # Store processed audio
        end = pos + n
        if end <= max_samples:
            audio_buf[pos:end] = chunk
        else:
            state['done'].set()
            return
        state['pos'] = end

        # Level metering
        rms = np.sqrt(np.mean(chunk ** 2))
        peak = np.max(np.abs(chunk))
        state['rms'] = rms
        state['peak'] = max(peak, state['peak'] * 0.95)

        # Onset detection
        if state['cooldown'] > 0:
            state['cooldown'] -= 1
            return

        energy = np.sum(chunk ** 2) / n
        state['energy_avg'] = state['energy_avg'] * 0.92 + energy * 0.08

        if (state['energy_avg'] > 1e-8 and
                energy > state['energy_avg'] * threshold):
            state['count'] += 1
            snr = energy / (state['energy_avg'] + 1e-10)
            state['onset_energies'].append(snr)
            state['cooldown'] = int(0.3 * rate / BLOCK)  # 300ms

            count = state['count']
            bar = '█' * count + '░' * max(0, num_presses - count)
            meter = level_bar(state['rms'], state['peak'])
            sys.stdout.write(
                f"\r  [{bar}] {count}/{num_presses}  "
                f"{meter}  SNR:{snr:.0f}x")
            sys.stdout.flush()

            if count >= num_presses:
                threading.Timer(0.3, state['done'].set).start()

    try:
        with sd.InputStream(device=device, channels=CHANNELS,
                            samplerate=rate, blocksize=BLOCK,
                            dtype='float32', callback=callback):
            # Show initial state
            sys.stdout.write(
                f"\r  [{'░' * num_presses}] 0/{num_presses}  "
                f"{'waiting for keystrokes...':40s}")
            sys.stdout.flush()
            state['done'].wait(timeout=max_duration)
    except sd.PortAudioError as e:
        print(f"\n  Audio error: {e}")
        return None, []

    print()  # newline after progress bar

    actual = state['pos']
    if state['count'] < num_presses:
        print(f"  ⚠ Only detected {state['count']}/{num_presses} "
              f"(timed out after {max_duration}s)")
        if state['count'] == 0:
            print(f"  ⚠ No keystrokes detected! Try:")
            print(f"    - Move mic closer to keyboard")
            print(f"    - Increase gain: --gain {gain * 2:.0f}")
            print(f"    - Lower threshold: --threshold {threshold * 0.6:.1f}")

    return audio_buf[:actual], state['onset_energies']


# ── Mic check ────────────────────────────────────────────────────────

def mic_check(device, rate, gain, hp_filter, duration=3):
    """Quick mic check — show live levels for a few seconds."""
    print("  Mic check: type a few keys and watch the level meter...")

    state = {'rms': 0.0, 'peak': 0.0, 'max_peak': 0.0}

    def callback(indata, frames, time_info, status):
        chunk = indata[:, 0].copy() * gain
        chunk = hp_filter.process(chunk)
        rms = np.sqrt(np.mean(chunk ** 2))
        peak = np.max(np.abs(chunk))
        state['rms'] = rms
        state['peak'] = max(peak, state['peak'] * 0.95)
        state['max_peak'] = max(state['max_peak'], peak)

    try:
        with sd.InputStream(device=device, channels=CHANNELS,
                            samplerate=rate, blocksize=BLOCK,
                            dtype='float32', callback=callback):
            end_time = time.time() + duration
            while time.time() < end_time:
                meter = level_bar(state['rms'], state['peak'])
                sys.stdout.write(f"\r  {meter}")
                sys.stdout.flush()
                time.sleep(0.05)
    except sd.PortAudioError as e:
        print(f"\n  Audio error: {e}")
        return False

    peak_db = 20 * np.log10(state['max_peak'] + 1e-10)
    print()

    if state['max_peak'] < 0.01:
        print(f"  ⚠ Very weak signal (peak: {peak_db:.0f}dB)")
        print(f"    Try: --gain {gain * 5:.0f}")
        return False
    elif state['max_peak'] < 0.05:
        print(f"  Signal OK but quiet (peak: {peak_db:.0f}dB)")
        suggested = gain * (0.3 / state['max_peak'])
        print(f"    Suggest: --gain {suggested:.0f}")
        return True
    else:
        print(f"  Signal good (peak: {peak_db:.0f}dB)")
        return True


# ── Main ─────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Collect keystroke audio for training")
    parser.add_argument('output_dir', nargs='?', default='keystroke_data',
                        help='Output directory (default: keystroke_data)')
    parser.add_argument('--keys', default='abcdefghijklmnopqrstuvwxyz ',
                        help='Keys to collect (default: a-z + space)')
    parser.add_argument('--presses', type=int, default=20,
                        help='Presses per key (default: 20)')
    parser.add_argument('--gain', type=float, default=10.0,
                        help='Software mic gain (default: 10.0)')
    parser.add_argument('--device', default=None,
                        help='Audio input device (index or name)')
    parser.add_argument('--threshold', type=float, default=5.0,
                        help='Onset detection threshold (default: 5.0)')
    parser.add_argument('--no-check', action='store_true',
                        help='Skip mic check')
    parser.add_argument('--list', action='store_true',
                        help='List audio devices and exit')
    args = parser.parse_args()

    if args.list:
        print(sd.query_devices())
        return

    device = args.device
    if device is not None:
        try:
            device = int(device)
        except ValueError:
            pass

    out = Path(args.output_dir)
    out.mkdir(exist_ok=True)

    keys = args.keys
    n = args.presses
    total = len(keys)
    gain = args.gain

    print(f"Collecting {n} presses for {total} keys → {out}/")
    print(f"Device: {device or 'default'}  Gain: {gain}x  "
          f"HP filter: 80Hz")
    print()

    hp_filter = HighPass(80, RATE)

    # Mic check
    if not args.no_check:
        ok = mic_check(device, RATE, gain, hp_filter)
        if not ok:
            resp = input("\n  Continue anyway? [y/N] ").strip().lower()
            if resp != 'y':
                return
        # Reset filter state
        hp_filter = HighPass(80, RATE)
        print()

    all_snrs = []

    for i, key in enumerate(keys):
        label = key if key != ' ' else 'space'
        print(f"[{i+1}/{total}] Key: {label}")
        input("  Press ENTER when ready, then start typing...")

        # Fresh filter per key to avoid state leakage
        hp_key = HighPass(80, RATE)
        audio, snrs = collect_key(
            label, n, device, RATE, gain, hp_key, args.threshold)
        if audio is None:
            continue

        all_snrs.extend(snrs)

        npy_path = out / f"key_{label}.npy"
        np.save(npy_path, audio)
        duration = len(audio) / RATE
        avg_snr = np.mean(snrs) if snrs else 0
        print(f"  Saved {npy_path} ({duration:.1f}s, "
              f"avg SNR: {avg_snr:.0f}x)\n")

    # Summary
    if all_snrs:
        print(f"Collection complete!")
        print(f"  Average SNR: {np.mean(all_snrs):.0f}x  "
              f"(min: {np.min(all_snrs):.0f}x, "
              f"max: {np.max(all_snrs):.0f}x)")
        if np.mean(all_snrs) < 8:
            print(f"  ⚠ Low SNR — consider increasing --gain "
                  f"or moving mic closer")
    print(f"\nRun: python train_model.py {out}")


if __name__ == "__main__":
    main()
