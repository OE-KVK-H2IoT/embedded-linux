#!/usr/bin/env python3
"""Real-time keystroke recognition from microphone.

Usage:
    python live_inference.py                      # defaults
    python live_inference.py --debug              # show energy + detections
    python live_inference.py --threshold 3.0      # lower = more sensitive
    python live_inference.py --device 4           # device index
"""

import argparse
import numpy as np
import sounddevice as sd
import pickle
import sys
import time

from features import (RATE, WINDOW_MS, PRE_ONSET_MS, extract_features)
from collect_keystrokes import HighPass

BLOCK = 480  # 10ms blocks


def main():
    parser = argparse.ArgumentParser(description="Live keystroke recognition")
    parser.add_argument('--model', default='keystroke_model.pkl',
                        help='Trained model path')
    parser.add_argument('--device', default=None,
                        help='Audio input device (index or name)')
    parser.add_argument('--threshold', type=float, default=5.0,
                        help='Onset energy ratio threshold')
    parser.add_argument('--min-energy', type=float, default=1e-5,
                        help='Minimum absolute energy to trigger onset')
    parser.add_argument('--cooldown-ms', type=int, default=400,
                        help='Cooldown between detections in ms (default: 400)')
    parser.add_argument('--gain', type=float, default=10.0,
                        help='Software mic gain (default: 10.0)')
    parser.add_argument('--confidence', type=float, default=0.2,
                        help='Minimum prediction confidence')
    parser.add_argument('--debug', action='store_true',
                        help='Show energy levels and detection events')
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

    with open(args.model, "rb") as f:
        model = pickle.load(f)

    # Audio processing — same as collection
    hp_filter = HighPass(80, RATE)
    gain = args.gain

    # How many samples to collect AFTER onset before classifying
    post_samples = int(RATE * WINDOW_MS / 1000)
    pre_samples = int(RATE * PRE_ONSET_MS / 1000)
    cooldown_blocks = int(args.cooldown_ms / 1000 * RATE / BLOCK)

    # Use a simple growing list instead of ring buffer —
    # easier to reason about, and we only need ~200ms of audio per keystroke
    state = {
        'energy_avg': 0.0,
        'cooldown': 0,
        'collecting': False,
        'onset_audio': [],       # chunks collected since onset
        'onset_pre': None,       # pre-onset chunk
        'samples_collected': 0,
        'prev_chunk': np.zeros(BLOCK, dtype=np.float32),
        'debug_peak': 0.0,
    }

    def audio_callback(indata, frames, time_info, status):
        audio = indata[:, 0].copy()
        audio *= gain
        audio = hp_filter.process(audio)
        n = len(audio)

        energy = np.sum(audio ** 2) / n

        if args.debug:
            state['debug_peak'] = max(state['debug_peak'] * 0.95, energy)

        # Phase 1: collecting post-onset audio
        if state['collecting']:
            state['onset_audio'].append(audio)
            state['samples_collected'] += n

            if state['samples_collected'] >= post_samples:
                # Assemble segment: pre-onset + onset + post-onset
                all_audio = np.concatenate(state['onset_audio'])
                pre = state['onset_pre']
                if pre is not None:
                    segment = np.concatenate([pre[-pre_samples:], all_audio])
                else:
                    segment = all_audio

                state['collecting'] = False
                state['onset_audio'] = []
                state['cooldown'] = cooldown_blocks

                # Classify
                try:
                    features = extract_features(segment)
                    prediction = model.predict(features.reshape(1, -1))[0]
                    proba = model.predict_proba(features.reshape(1, -1)).max()

                    if args.debug:
                        sys.stderr.write(
                            f"  CLASSIFY: '{prediction}' "
                            f"conf={proba:.0%}\n")
                        sys.stderr.flush()

                    if proba > args.confidence:
                        display = prediction if prediction != 'space' else ' '
                        sys.stdout.write(display)
                        sys.stdout.flush()
                except Exception as e:
                    if args.debug:
                        sys.stderr.write(f"  ERROR: {e}\n")
                        sys.stderr.flush()
            return

        # Phase 2: cooldown after classification
        if state['cooldown'] > 0:
            state['cooldown'] -= 1
            state['prev_chunk'] = audio.copy()
            state['energy_avg'] = state['energy_avg'] * 0.92 + energy * 0.08
            return

        # Phase 3: onset detection
        state['energy_avg'] = state['energy_avg'] * 0.92 + energy * 0.08

        if args.debug and int(time.time() * 4) % 4 == 0:
            ratio = energy / (state['energy_avg'] + 1e-10)
            bar_len = min(50, int(ratio * 5))
            bar = '█' * bar_len
            sys.stderr.write(
                f"\r  energy={energy:.2e} avg={state['energy_avg']:.2e} "
                f"ratio={ratio:.1f}x [{bar:<50s}]")
            sys.stderr.flush()

        ratio = energy / (state['energy_avg'] + 1e-10)
        if (energy > args.min_energy and
                ratio > args.threshold and
                state['energy_avg'] > 1e-8):
            # Onset detected! Start collecting
            if args.debug:
                sys.stderr.write(
                    f"\n  ONSET: ratio={ratio:.1f}x "
                    f"energy={energy:.2e}\n")
                sys.stderr.flush()

            state['collecting'] = True
            state['onset_pre'] = state['prev_chunk'].copy()
            state['onset_audio'] = [audio.copy()]
            state['samples_collected'] = n

        state['prev_chunk'] = audio.copy()

    print(f"Model:      {args.model}")
    print(f"Device:     {device or 'default'}")
    print(f"Gain:       {gain}x + 80Hz high-pass")
    print(f"Threshold:  {args.threshold}x ratio, {args.min_energy:.0e} min energy")
    print(f"Cooldown:   {args.cooldown_ms}ms")
    print(f"Confidence: {args.confidence}")
    if args.debug:
        print("Debug:      ON (energy levels on stderr)")
    print()
    print("Listening... type on the keyboard near the microphone.")
    print("Predicted keys appear below (Ctrl+C to stop):\n")

    with sd.InputStream(device=device, channels=1, samplerate=RATE,
                        blocksize=BLOCK, dtype='float32',
                        callback=audio_callback):
        try:
            while True:
                sd.sleep(100)
        except KeyboardInterrupt:
            pass
    print("\nStopped.")


if __name__ == "__main__":
    main()
