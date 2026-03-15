#!/usr/bin/env python3
"""Real-time keystroke recognition from microphone.

Usage:
    python live_inference.py                  # use default model + device
    python live_inference.py --model m.pkl    # specify model file
    python live_inference.py --device hw:1,0  # specify ALSA device
"""

import argparse
import numpy as np
import sounddevice as sd
import pickle
import sys
from scipy.fft import rfft

RATE = 48000
BLOCK = 2400  # 50ms blocks


def extract_features(audio, onset, rate, window_ms=80, n_mels=40):
    """Extract mel-spectrogram features from a keystroke."""
    window = int(rate * window_ms / 1000)
    segment = audio[onset:onset + window]
    if len(segment) < window:
        segment = np.pad(segment, (0, window - len(segment)))

    frame_len = int(rate * 0.004)
    hop = frame_len // 2
    n_frames = (window - frame_len) // hop + 1

    spec = np.zeros((frame_len // 2 + 1, n_frames))
    for i in range(n_frames):
        frame = segment[i * hop: i * hop + frame_len]
        frame = frame * np.hanning(len(frame))
        fft = np.abs(rfft(frame))
        spec[:, i] = 20 * np.log10(fft + 1e-10)

    freq_bins = spec.shape[0]
    mel_bins = np.logspace(np.log10(1), np.log10(freq_bins), n_mels + 1,
                           dtype=int)
    mel_spec = np.zeros((n_mels, n_frames))
    for m in range(n_mels):
        lo, hi = mel_bins[m], mel_bins[m + 1]
        if hi <= lo:
            hi = lo + 1
        mel_spec[m, :] = np.mean(spec[lo:hi, :], axis=0)

    return mel_spec.flatten()


def main():
    parser = argparse.ArgumentParser(description="Live keystroke recognition")
    parser.add_argument('--model', default='keystroke_model.pkl',
                        help='Path to trained model (default: keystroke_model.pkl)')
    parser.add_argument('--device', default='hw:1,0',
                        help='ALSA input device (default: hw:1,0)')
    parser.add_argument('--threshold', type=float, default=5.0,
                        help='Energy spike threshold (default: 5.0)')
    parser.add_argument('--confidence', type=float, default=0.3,
                        help='Minimum prediction confidence (default: 0.3)')
    args = parser.parse_args()

    # Load model
    with open(args.model, "rb") as f:
        model = pickle.load(f)

    # State for onset detection
    energy_avg = 0
    cooldown = 0
    audio_buffer = np.zeros(RATE, dtype=np.float32)  # 1s ring buffer
    buf_pos = 0

    def audio_callback(indata, frames, time_info, status):
        nonlocal energy_avg, cooldown, buf_pos

        audio = indata[:, 0].copy()
        energy = np.sum(audio ** 2) / len(audio)
        energy_avg = energy_avg * 0.9 + energy * 0.1

        # Store in ring buffer
        n = len(audio)
        end = buf_pos + n
        if end <= len(audio_buffer):
            audio_buffer[buf_pos:end] = audio
        else:
            wrap = end - len(audio_buffer)
            audio_buffer[buf_pos:] = audio[:n - wrap]
            audio_buffer[:wrap] = audio[n - wrap:]
        buf_pos = end % len(audio_buffer)

        if cooldown > 0:
            cooldown -= 1
            return

        # Onset detection: energy spike > threshold * average
        if energy > energy_avg * args.threshold and energy_avg > 1e-8:
            cooldown = 4  # ~200ms cooldown

            # Extract features from the last 80ms
            window = int(RATE * 0.08)
            start = (buf_pos - window) % len(audio_buffer)
            if start + window <= len(audio_buffer):
                segment = audio_buffer[start:start + window]
            else:
                segment = np.concatenate([
                    audio_buffer[start:],
                    audio_buffer[:window - (len(audio_buffer) - start)]
                ])

            features = extract_features(segment, 0, RATE)
            prediction = model.predict(features.reshape(1, -1))[0]
            confidence = model.predict_proba(features.reshape(1, -1)).max()

            if confidence > args.confidence:
                sys.stdout.write(prediction)
                sys.stdout.flush()

    print(f"Model: {args.model}")
    print(f"Device: {args.device}")
    print("Listening... type on the keyboard near the microphone")
    print("Predicted keys will appear below:\n")

    with sd.InputStream(device=args.device, channels=1, samplerate=RATE,
                        blocksize=BLOCK, callback=audio_callback):
        try:
            input()  # Block until Enter
        except KeyboardInterrupt:
            pass
    print("\nStopped.")


if __name__ == "__main__":
    main()
