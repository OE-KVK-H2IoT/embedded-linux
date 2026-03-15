#!/usr/bin/env python3
"""Train keystroke classifier from collected audio data.

Usage:
    python train_model.py                      # use default keystroke_data/
    python train_model.py path/to/data_dir     # specify data directory
"""

import sys
import pickle
import numpy as np
from pathlib import Path
from scipy.fft import rfft
from sklearn.ensemble import RandomForestClassifier
from sklearn.model_selection import cross_val_score
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import Pipeline

RATE = 48000


# ── Onset detection ─────────────────────────────────────────────────

def detect_onsets(audio, rate, threshold=3.0, min_gap_ms=200):
    """Find keystroke onset positions using energy ratio."""
    frame_ms = 5
    frame_len = int(rate * frame_ms / 1000)
    hop = frame_len // 2

    # Short-term energy
    energy = []
    for i in range(0, len(audio) - frame_len, hop):
        e = np.sum(audio[i:i + frame_len] ** 2)
        energy.append(e)
    energy = np.array(energy)

    # Running average (100ms window)
    avg_len = int(100 / frame_ms)
    avg = np.convolve(energy, np.ones(avg_len) / avg_len, mode='same')

    # Find peaks where energy exceeds threshold * average
    ratio = energy / (avg + 1e-10)
    peaks = np.where(ratio > threshold)[0]

    # Merge nearby peaks (min_gap)
    min_gap = int(min_gap_ms / frame_ms * 2)
    onsets = []
    last = -min_gap
    for p in peaks:
        if p - last >= min_gap:
            onsets.append(p * hop)
            last = p

    return onsets


# ── Feature extraction ──────────────────────────────────────────────

def extract_features(audio, onset, rate, window_ms=80, n_mels=40):
    """Extract mel-spectrogram features from a keystroke."""
    window = int(rate * window_ms / 1000)
    segment = audio[onset:onset + window]
    if len(segment) < window:
        segment = np.pad(segment, (0, window - len(segment)))

    # STFT: 4ms frames, 2ms hop
    frame_len = int(rate * 0.004)
    hop = frame_len // 2
    n_frames = (window - frame_len) // hop + 1

    spec = np.zeros((frame_len // 2 + 1, n_frames))
    for i in range(n_frames):
        frame = segment[i * hop: i * hop + frame_len]
        frame = frame * np.hanning(len(frame))
        fft = np.abs(rfft(frame))
        spec[:, i] = 20 * np.log10(fft + 1e-10)

    # Mel-scale binning (simplified: log-spaced frequency bins)
    freq_bins = spec.shape[0]
    mel_bins = np.logspace(np.log10(1), np.log10(freq_bins), n_mels + 1,
                           dtype=int)
    mel_spec = np.zeros((n_mels, n_frames))
    for m in range(n_mels):
        lo, hi = mel_bins[m], mel_bins[m + 1]
        if hi <= lo:
            hi = lo + 1
        mel_spec[m, :] = np.mean(spec[lo:hi, :], axis=0)

    return mel_spec.flatten()  # fixed-length feature vector


# ── Dataset builder ─────────────────────────────────────────────────

def build_dataset(data_dir):
    """Load all recorded keys, detect onsets, extract features."""
    data_dir = Path(data_dir)
    X, y = [], []

    for npy_file in sorted(data_dir.glob("key_*.npy")):
        label = npy_file.stem.replace("key_", "")
        audio = np.load(npy_file)

        onsets = detect_onsets(audio, RATE)
        print(f"  {label}: {len(onsets)} keystrokes detected")

        for onset in onsets:
            features = extract_features(audio, onset, RATE)
            X.append(features)
            y.append(label)

    return np.array(X), np.array(y)


# ── Data augmentation ───────────────────────────────────────────────

def augment(segment, rate):
    """Create augmented copies of a keystroke segment."""
    augmented = [segment]  # original

    # Time shift: +/- 5ms
    shift = int(rate * 0.005)
    augmented.append(np.roll(segment, shift))
    augmented.append(np.roll(segment, -shift))

    # Add slight noise
    noise = np.random.randn(len(segment)) * 0.001
    augmented.append(segment + noise)

    # Slight gain variation (+/- 10%)
    augmented.append(segment * 1.1)
    augmented.append(segment * 0.9)

    return augmented


# ── Main ────────────────────────────────────────────────────────────

if __name__ == "__main__":
    data_dir = sys.argv[1] if len(sys.argv) > 1 else "keystroke_data"

    print(f"Loading data from {data_dir}/ ...")
    X, y = build_dataset(data_dir)
    print(f"Dataset: {len(X)} samples, {len(set(y))} classes\n")

    # Train with cross-validation
    pipeline = Pipeline([
        ("scaler", StandardScaler()),
        ("clf", RandomForestClassifier(n_estimators=100, random_state=42))
    ])

    scores = cross_val_score(pipeline, X, y, cv=5, scoring="accuracy")
    print(f"Cross-validation accuracy: {scores.mean():.1%} (+/- {scores.std():.1%})")

    # Train final model on all data
    pipeline.fit(X, y)

    # Save model
    with open("keystroke_model.pkl", "wb") as f:
        pickle.dump(pipeline, f)
    print("Model saved to keystroke_model.pkl")
