#!/usr/bin/env python3
"""Shared feature extraction for keystroke recognition.

Used by both train_model.py and live_inference.py to ensure
identical features at training and inference time.
"""

import numpy as np
from scipy.fft import rfft

RATE = 48000

# ── Feature parameters ──────────────────────────────────────────────
WINDOW_MS = 100       # capture window after onset (keystroke is ~50ms,
                      #   extra for resonance tail)
PRE_ONSET_MS = 5      # include a tiny bit before onset for attack
N_FFT = 512           # FFT size (~10.7ms at 48kHz)
HOP_MS = 2            # STFT hop
N_MELS = 32           # mel bands
F_MIN = 100           # Hz — below this is just rumble
F_MAX = 12000         # Hz — above this is just hiss


def mel_filterbank(n_fft, rate, n_mels, f_min, f_max):
    """Build a proper mel filterbank matrix (triangular filters)."""
    def hz_to_mel(f):
        return 2595 * np.log10(1 + f / 700)

    def mel_to_hz(m):
        return 700 * (10 ** (m / 2595) - 1)

    mel_min = hz_to_mel(f_min)
    mel_max = hz_to_mel(f_max)
    mel_points = np.linspace(mel_min, mel_max, n_mels + 2)
    hz_points = mel_to_hz(mel_points)

    # Convert Hz to FFT bin indices
    bin_points = np.floor((n_fft + 1) * hz_points / rate).astype(int)

    fb = np.zeros((n_mels, n_fft // 2 + 1))
    for m in range(n_mels):
        lo, center, hi = bin_points[m], bin_points[m + 1], bin_points[m + 2]
        # Rising slope
        for k in range(lo, center):
            if center > lo:
                fb[m, k] = (k - lo) / (center - lo)
        # Falling slope
        for k in range(center, hi):
            if hi > center:
                fb[m, k] = (hi - k) / (hi - center)

    return fb


# Pre-compute filterbank (module-level, computed once)
_FILTERBANK = None


def _get_filterbank():
    global _FILTERBANK
    if _FILTERBANK is None:
        _FILTERBANK = mel_filterbank(N_FFT, RATE, N_MELS, F_MIN, F_MAX)
    return _FILTERBANK


def extract_features(segment):
    """Extract mel-spectrogram + summary features from a keystroke segment.

    Args:
        segment: 1D float32 array, already windowed around the onset.
                 Expected length: ~(WINDOW_MS + PRE_ONSET_MS) * RATE / 1000

    Returns:
        1D feature vector (fixed length regardless of minor size variation).
    """
    window = int(RATE * (WINDOW_MS + PRE_ONSET_MS) / 1000)

    # Pad or truncate to exact window
    if len(segment) < window:
        segment = np.pad(segment, (0, window - len(segment)))
    else:
        segment = segment[:window]

    # Normalize amplitude (important for consistency across sessions)
    peak = np.max(np.abs(segment))
    if peak > 1e-6:
        segment = segment / peak

    fb = _get_filterbank()
    hop = int(RATE * HOP_MS / 1000)
    n_frames = (window - N_FFT) // hop + 1

    # STFT → power spectrum → mel
    hann = np.hanning(N_FFT)
    spec = np.zeros((N_FFT // 2 + 1, n_frames))
    for i in range(n_frames):
        frame = segment[i * hop: i * hop + N_FFT]
        if len(frame) < N_FFT:
            frame = np.pad(frame, (0, N_FFT - len(frame)))
        fft = np.abs(rfft(frame * hann)) ** 2  # power spectrum
        spec[:, i] = fft

    # Apply mel filterbank → log scale
    mel_spec = fb @ spec
    mel_spec = 10 * np.log10(mel_spec + 1e-10)

    # ── Features ────────────────────────────────────────────────────
    features = []

    # 1. Flattened mel spectrogram (main feature)
    features.append(mel_spec.flatten())

    # 2. Temporal envelope: mean energy per frame (captures attack/decay shape)
    frame_energy = np.mean(mel_spec, axis=0)
    features.append(frame_energy)

    # 3. Spectral centroid per frame (which frequencies dominate over time)
    freqs = np.arange(N_MELS)
    total = np.sum(mel_spec - mel_spec.min(), axis=0) + 1e-10
    centroid = np.sum(freqs[:, None] * (mel_spec - mel_spec.min()), axis=0) / total
    features.append(centroid)

    # 4. Delta (first derivative of mel bands, captures transitions)
    delta = np.diff(mel_spec, axis=1)
    features.append(delta.flatten())

    return np.concatenate(features)


def detect_onsets(audio, rate=RATE, threshold=3.0, min_gap_ms=200):
    """Find keystroke onset positions using energy ratio."""
    frame_ms = 5
    frame_len = int(rate * frame_ms / 1000)
    hop = frame_len // 2

    energy = []
    for i in range(0, len(audio) - frame_len, hop):
        e = np.sum(audio[i:i + frame_len] ** 2)
        energy.append(e)
    energy = np.array(energy)

    # Running average (100ms window)
    avg_len = int(100 / frame_ms)
    avg = np.convolve(energy, np.ones(avg_len) / avg_len, mode='same')

    ratio = energy / (avg + 1e-10)
    peaks = np.where(ratio > threshold)[0]

    min_gap = int(min_gap_ms / frame_ms * 2)
    onsets = []
    last = -min_gap
    for p in peaks:
        if p - last >= min_gap:
            onsets.append(p * hop)
            last = p

    return onsets


def extract_keystroke_segment(audio, onset, rate=RATE):
    """Extract the audio segment around an onset for feature extraction."""
    pre = int(rate * PRE_ONSET_MS / 1000)
    post = int(rate * WINDOW_MS / 1000)
    start = max(0, onset - pre)
    end = onset + post
    return audio[start:end]
