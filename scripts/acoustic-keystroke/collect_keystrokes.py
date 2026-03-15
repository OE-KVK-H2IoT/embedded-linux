#!/usr/bin/env python3
"""Record keystrokes with synchronized audio for training data."""

import sys
import numpy as np
import sounddevice as sd
from pathlib import Path

RATE = 48000
CHANNELS = 1
DEVICE = "hw:1,0"  # adjust for your setup


def collect(output_dir: str, keys: str = "abcdefghijklmnopqrstuvwxyz "):
    """Record keystrokes for each key in the list."""
    out = Path(output_dir)
    out.mkdir(exist_ok=True)

    print(f"Will collect keystrokes for: {keys}")
    print("Press each key 20 times when prompted.\n")

    for key in keys:
        label = key if key != " " else "space"
        print(f"── Key: [{label}] ── Press it 20 times, ~1 per second ──")
        input("  Press ENTER when ready, then start typing...")

        # Record 25 seconds (buffer for 20 presses + gaps)
        duration = 25
        audio = sd.rec(int(duration * RATE), samplerate=RATE,
                       channels=CHANNELS, dtype="float32",
                       device=DEVICE)
        sd.wait()

        # Save raw audio
        wav_path = out / f"key_{label}.npy"
        np.save(wav_path, audio.flatten())
        print(f"  Saved {wav_path} ({len(audio)} samples)\n")

    print("Collection complete. Run train_model.py next.")


if __name__ == "__main__":
    collect(sys.argv[1] if len(sys.argv) > 1 else "keystroke_data")
