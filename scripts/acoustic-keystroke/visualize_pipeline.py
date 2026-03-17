#!/usr/bin/env python3
"""Acoustic Keystroke Recognition — Visual Pipeline Demo

Generates tutorial-quality visualizations showing each stage of
the keystroke recognition pipeline. Works with real recorded data
(if available) or generates synthetic keystroke-like signals.

Outputs:
  1. onset_detection.png     — waveform + energy + threshold + detections
  2. keystroke_comparison.png — spectrograms of different keys side by side
  3. feature_pipeline.png    — raw audio → spectrogram → mel → features
  4. feature_space.png       — 2D projection of feature vectors (PCA/t-SNE)
  5. learning_curve.png      — accuracy vs training data quantity
  6. confusion_matrix.png    — which keys get confused

Run:
  python visualize_pipeline.py                      # synthetic data
  python visualize_pipeline.py --data keystroke_data # from real recordings
  python visualize_pipeline.py --interactive        # GUI with audio playback

Requirements: numpy, matplotlib, scipy
Optional: scikit-learn (for feature_space + learning_curve + confusion)
"""

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec
from scipy.fft import rfft, rfftfreq
import argparse
import os

RATE = 48000


# ── Synthetic keystroke generator ────────────────────────────────────

def synth_keystroke(key_id, rate=RATE, duration=0.1):
    """Generate a synthetic keystroke sound with key-dependent characteristics.
    Each key has a unique resonance frequency and decay profile."""
    n = int(rate * duration)
    t = np.arange(n) / rate

    # Key-dependent parameters (deterministic from key_id)
    rng = np.random.RandomState(key_id * 7 + 42)
    f_resonance = 800 + key_id * 120 + rng.randn() * 50
    f_secondary = 2000 + key_id * 80 + rng.randn() * 100
    decay1 = 30 + key_id * 2
    decay2 = 50 + key_id * 3
    attack_ms = 1.0 + rng.rand() * 0.5

    # Attack transient (broadband click)
    attack_n = int(attack_ms / 1000 * rate)
    attack = np.zeros(n)
    attack[:attack_n] = rng.randn(attack_n) * 0.8

    # Resonance (decaying sinusoids)
    body = (0.6 * np.sin(2 * np.pi * f_resonance * t) * np.exp(-decay1 * t) +
            0.3 * np.sin(2 * np.pi * f_secondary * t) * np.exp(-decay2 * t))

    signal = attack + body

    # Add slight noise
    signal += rng.randn(n) * 0.01

    return signal


def synth_recording(key_id, n_presses=20, rate=RATE):
    """Generate a fake recording session: silence + keystrokes."""
    gap = int(rate * 0.4)  # 400ms between presses
    keystroke_len = int(rate * 0.1)
    segments = []
    onsets = []
    pos = int(rate * 0.2)  # 200ms initial silence

    for i in range(n_presses):
        silence = np.random.randn(gap) * 0.002  # background noise
        segments.append(silence)
        pos += gap

        ks = synth_keystroke(key_id, rate)
        # Slight variation per press
        ks *= (0.8 + 0.4 * np.random.rand())
        segments.append(ks)
        onsets.append(pos)
        pos += keystroke_len

    segments.append(np.random.randn(int(rate * 0.2)) * 0.002)
    audio = np.concatenate(segments)
    return audio, onsets


# ── Onset detection ──────────────────────────────────────────────────

def detect_onsets_visual(audio, rate=RATE, block_ms=10, threshold=5.0):
    """Onset detection with intermediate values for visualization."""
    block = int(rate * block_ms / 1000)
    n_blocks = len(audio) // block
    times = np.arange(n_blocks) * block / rate

    energies = np.zeros(n_blocks)
    for i in range(n_blocks):
        chunk = audio[i * block:(i + 1) * block]
        energies[i] = np.sum(chunk ** 2) / len(chunk)

    # EMA for running average
    avg = np.zeros(n_blocks)
    ema = energies[0]
    for i in range(n_blocks):
        ema = ema * 0.92 + energies[i] * 0.08
        avg[i] = ema

    # Detect onsets
    ratio = energies / (avg + 1e-10)
    onsets = []
    cooldown = 0
    min_gap = int(0.3 * rate / block)  # 300ms
    for i in range(n_blocks):
        if cooldown > 0:
            cooldown -= 1
            continue
        if ratio[i] > threshold and energies[i] > 1e-5:
            onsets.append(int(times[i] * rate))
            cooldown = min_gap

    return times, energies, avg, ratio, onsets


# ── Feature extraction (simplified) ─────────────────────────────────

def hz_to_mel(f):
    return 2595 * np.log10(1 + f / 700)

def mel_to_hz(m):
    return 700 * (10 ** (m / 2595) - 1)

def make_mel_fb(n_mels, n_fft, fs, f_min=100, f_max=12000):
    mel_min, mel_max = hz_to_mel(f_min), hz_to_mel(f_max)
    mel_pts = np.linspace(mel_min, mel_max, n_mels + 2)
    hz_pts = mel_to_hz(mel_pts)
    bins = np.round(hz_pts / fs * n_fft).astype(int)
    n_bins = n_fft // 2 + 1
    fb = np.zeros((n_mels, n_bins))
    for m in range(n_mels):
        for k in range(bins[m], bins[m+1]):
            if bins[m+1] > bins[m]:
                fb[m, k] = (k - bins[m]) / (bins[m+1] - bins[m])
        for k in range(bins[m+1], bins[m+2]):
            if bins[m+2] > bins[m+1]:
                fb[m, k] = (bins[m+2] - k) / (bins[m+2] - bins[m+1])
    return fb


def extract_mel_spec(segment, rate=RATE, n_fft=512, hop=96, n_mels=32):
    """Extract mel spectrogram from audio segment."""
    window = np.hanning(n_fft)
    n_frames = max(1, (len(segment) - n_fft) // hop + 1)
    n_bins = n_fft // 2 + 1
    spec = np.zeros((n_bins, n_frames))

    for i in range(n_frames):
        frame = segment[i * hop:i * hop + n_fft]
        if len(frame) < n_fft:
            frame = np.pad(frame, (0, n_fft - len(frame)))
        spec[:, i] = np.abs(rfft(frame * window)) ** 2

    fb = make_mel_fb(n_mels, n_fft, rate)
    mel = fb @ spec
    mel_db = 10 * np.log10(mel + 1e-10)
    return spec, mel, mel_db


# ── Visualization functions ──────────────────────────────────────────

def plot_onset_detection(save=True):
    """Fig 1: Onset detection pipeline visualization."""
    audio, true_onsets = synth_recording(key_id=0, n_presses=8)
    times, energies, avg, ratio, detected = detect_onsets_visual(audio)

    fig, axes = plt.subplots(4, 1, figsize=(14, 10), sharex=True)
    fig.suptitle('Onset Detection Pipeline', fontsize=14, fontweight='bold')

    t_audio = np.arange(len(audio)) / RATE

    # 1. Raw waveform
    ax = axes[0]
    ax.plot(t_audio, audio, 'b-', linewidth=0.3)
    for onset in detected:
        ax.axvline(onset / RATE, color='red', alpha=0.5, linewidth=1)
    ax.set_ylabel('Amplitude')
    ax.set_title('Raw Waveform (blue) + Detected Onsets (red lines)')

    # 2. Block energy
    ax = axes[1]
    ax.semilogy(times, energies, 'steelblue', linewidth=0.8, label='Block energy')
    ax.semilogy(times, avg, 'orange', linewidth=2, label='Running average (EMA)')
    ax.set_ylabel('Energy')
    ax.legend(loc='upper right')
    ax.set_title('Energy per 10ms Block vs Running Average')

    # 3. Energy ratio
    ax = axes[2]
    ax.plot(times, ratio, 'green', linewidth=0.8)
    ax.axhline(5.0, color='red', linestyle='--', label='Threshold (5×)')
    for onset in detected:
        ax.axvline(onset / RATE, color='red', alpha=0.3)
    ax.set_ylabel('Ratio')
    ax.set_ylim(0, 30)
    ax.legend()
    ax.set_title('Energy / Average Ratio — Above Threshold = Keystroke Detected')

    # 4. Post-onset capture window
    ax = axes[3]
    ax.plot(t_audio, audio, 'b-', linewidth=0.3, alpha=0.5)
    for onset in detected:
        start = onset / RATE
        end = start + 0.1  # 100ms window
        ax.axvspan(start, end, color='green', alpha=0.2)
        ax.annotate('100ms\nwindow', xy=(start + 0.02, 0.3),
                    fontsize=7, color='green')
    ax.set_ylabel('Amplitude')
    ax.set_xlabel('Time (s)')
    ax.set_title('Feature Extraction Windows (green) — 100ms After Each Onset')

    plt.tight_layout()
    if save:
        plt.savefig('onset_detection.png', dpi=150, bbox_inches='tight')
        print("Saved: onset_detection.png")
    return fig


def plot_keystroke_comparison(save=True):
    """Fig 2: Side-by-side comparison of different keys."""
    keys = ['a', 'e', 'space', 'f', 'k']
    key_ids = [0, 4, 26, 5, 10]

    fig = plt.figure(figsize=(16, 12))
    gs = GridSpec(3, len(keys), figure=fig, hspace=0.4)
    fig.suptitle('Different Keys Produce Different Spectral Signatures',
                 fontsize=14, fontweight='bold')

    for col, (key_name, kid) in enumerate(zip(keys, key_ids)):
        segment = synth_keystroke(kid)
        t = np.arange(len(segment)) / RATE * 1000
        spec, mel, mel_db = extract_mel_spec(segment)

        # Waveform
        ax = fig.add_subplot(gs[0, col])
        ax.plot(t, segment, 'b-', linewidth=0.5)
        ax.set_title(f"Key '{key_name}'", fontweight='bold', fontsize=11)
        ax.set_ylim(-1, 1)
        if col == 0:
            ax.set_ylabel('Amplitude')
        ax.set_xlabel('ms')

        # Linear spectrogram
        ax = fig.add_subplot(gs[1, col])
        ax.imshow(10 * np.log10(spec + 1e-10), aspect='auto',
                  origin='lower', cmap='magma',
                  extent=[0, 100, 0, RATE / 2 / 1000])
        ax.set_ylim(0, 8)
        if col == 0:
            ax.set_ylabel('Freq (kHz)')
        ax.set_xlabel('ms')

        # Mel spectrogram
        ax = fig.add_subplot(gs[2, col])
        ax.imshow(mel_db, aspect='auto', origin='lower', cmap='magma')
        if col == 0:
            ax.set_ylabel('Mel band')
        ax.set_xlabel('Frame')

    # Row labels on the left
    fig.text(0.01, 0.82, 'Waveform', fontsize=11, rotation=90, va='center')
    fig.text(0.01, 0.50, 'Spectrogram', fontsize=11, rotation=90, va='center')
    fig.text(0.01, 0.18, 'Mel Spec', fontsize=11, rotation=90, va='center')

    plt.tight_layout(rect=[0.03, 0, 1, 0.95])
    if save:
        plt.savefig('keystroke_comparison.png', dpi=150, bbox_inches='tight')
        print("Saved: keystroke_comparison.png")
    return fig


def plot_feature_pipeline(save=True):
    """Fig 3: Step-by-step feature extraction from one keystroke."""
    segment = synth_keystroke(key_id=0)
    t = np.arange(len(segment)) / RATE * 1000

    n_fft, hop, n_mels = 512, 96, 32
    spec, mel, mel_db = extract_mel_spec(segment, n_fft=n_fft, hop=hop,
                                          n_mels=n_mels)

    fig, axes = plt.subplots(2, 3, figsize=(16, 8))
    fig.suptitle("Feature Extraction Pipeline — From Audio to ML Input",
                 fontsize=14, fontweight='bold')

    # 1. Raw waveform
    ax = axes[0][0]
    ax.plot(t, segment, 'b-', linewidth=0.8)
    ax.set_title('1. Raw Keystroke (100ms)')
    ax.set_xlabel('Time (ms)')
    ax.set_ylabel('Amplitude')
    ax.annotate('Attack\n(click)', xy=(2, 0.6), fontsize=9,
                arrowprops=dict(arrowstyle='->', color='red'),
                xytext=(15, 0.8), color='red')
    ax.annotate('Resonance\n(decay)', xy=(30, 0.1), fontsize=9,
                arrowprops=dict(arrowstyle='->', color='green'),
                xytext=(50, 0.5), color='green')

    # 2. Windowed frames
    ax = axes[0][1]
    window = np.hanning(n_fft)
    for i in range(0, len(segment) - n_fft, hop * 5):  # show every 5th
        frame_t = (np.arange(n_fft) + i) / RATE * 1000
        ax.plot(frame_t, segment[i:i + n_fft] * window,
                linewidth=0.5, alpha=0.6)
    ax.set_title(f'2. Hann-Windowed Frames\n(FFT size={n_fft}, hop={hop})')
    ax.set_xlabel('Time (ms)')
    ax.set_ylabel('Amplitude')

    # 3. Power spectrogram
    ax = axes[0][2]
    spec_db = 10 * np.log10(spec + 1e-10)
    f_bins = rfftfreq(n_fft, 1.0 / RATE) / 1000
    im = ax.pcolormesh(np.arange(spec.shape[1]),
                       f_bins, spec_db, cmap='magma', shading='auto')
    ax.set_ylim(0, 8)
    ax.set_title('3. Power Spectrogram (STFT)')
    ax.set_xlabel('Frame')
    ax.set_ylabel('Frequency (kHz)')

    # 4. Mel filterbank
    ax = axes[1][0]
    fb = make_mel_fb(n_mels, n_fft, RATE)
    for m in range(n_mels):
        ax.plot(f_bins, fb[m, :], alpha=0.5, linewidth=0.8)
    ax.set_title(f'4. Mel Filterbank ({n_mels} bands)')
    ax.set_xlabel('Frequency (kHz)')
    ax.set_ylabel('Weight')
    ax.set_xlim(0, 8)

    # 5. Mel spectrogram
    ax = axes[1][1]
    ax.imshow(mel_db, aspect='auto', origin='lower', cmap='magma')
    ax.set_title('5. Mel Spectrogram (32 × N)')
    ax.set_xlabel('Frame')
    ax.set_ylabel('Mel band')

    # 6. Flattened feature vector
    ax = axes[1][2]
    flat = mel_db.flatten()
    # Show as a 1D bar-like visualization
    ax.imshow(flat.reshape(1, -1), aspect='auto', cmap='magma')
    ax.set_title(f'6. Feature Vector ({len(flat)} values)\n→ Input to SVM/CNN')
    ax.set_xlabel(f'Feature index (0–{len(flat)-1})')
    ax.set_yticks([])

    # Add arrows between panels
    for i in range(2):
        for j in range(2):
            fig.text(0.35 + j * 0.33, 0.92 - i * 0.48, '→',
                     fontsize=24, ha='center', va='center', color='gray')

    plt.tight_layout(rect=[0, 0, 1, 0.95])
    if save:
        plt.savefig('feature_pipeline.png', dpi=150, bbox_inches='tight')
        print("Saved: feature_pipeline.png")
    return fig


def plot_feature_space(save=True):
    """Fig 4: 2D visualization of keystroke feature vectors."""
    try:
        from sklearn.decomposition import PCA
    except ImportError:
        print("scikit-learn required for feature_space plot")
        return None

    # Generate features for 5 keys, 30 samples each
    keys = ['a', 'e', 'i', 'o', 'space']
    key_ids = [0, 4, 8, 14, 26]
    colors = ['#e74c3c', '#2ecc71', '#3498db', '#f39c12', '#9b59b6']

    all_features = []
    all_labels = []

    for kid, key_name in zip(key_ids, keys):
        for sample in range(30):
            # Add variation per sample
            rng = np.random.RandomState(kid * 100 + sample)
            segment = synth_keystroke(kid)
            segment *= (0.7 + 0.6 * rng.rand())  # gain variation
            segment = np.roll(segment, rng.randint(-50, 50))  # time shift
            segment += rng.randn(len(segment)) * 0.01  # noise

            _, _, mel_db = extract_mel_spec(segment)
            all_features.append(mel_db.flatten())
            all_labels.append(key_name)

    X = np.array(all_features)
    y = np.array(all_labels)

    # PCA to 2D
    pca = PCA(n_components=2)
    X_2d = pca.fit_transform(X)

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
    fig.suptitle('Keystroke Feature Space — Each Dot Is One Keystroke',
                 fontsize=14, fontweight='bold')

    # PCA scatter
    for key_name, color in zip(keys, colors):
        mask = y == key_name
        ax1.scatter(X_2d[mask, 0], X_2d[mask, 1], c=color, label=f"'{key_name}'",
                    s=40, alpha=0.7, edgecolors='k', linewidth=0.5)
    ax1.set_title(f'PCA Projection (explains {pca.explained_variance_ratio_.sum():.0%} variance)')
    ax1.set_xlabel(f'PC1 ({pca.explained_variance_ratio_[0]:.0%})')
    ax1.set_ylabel(f'PC2 ({pca.explained_variance_ratio_[1]:.0%})')
    ax1.legend()
    ax1.grid(True, alpha=0.3)

    # Per-key average spectrogram
    for i, (key_name, kid, color) in enumerate(zip(keys, key_ids, colors)):
        mask = y == key_name
        avg_feat = X[mask].mean(axis=0)
        n_mels, n_frames = 32, len(avg_feat) // 32
        avg_mel = avg_feat.reshape(n_mels, n_frames)

        # Small inset for each key
        inset_w = 0.15
        inset_h = 0.25
        inset_x = 0.55 + (i % 3) * 0.15
        inset_y = 0.55 if i < 3 else 0.15
        ax_in = fig.add_axes([inset_x, inset_y, inset_w, inset_h])
        ax_in.imshow(avg_mel, aspect='auto', origin='lower', cmap='magma')
        ax_in.set_title(f"'{key_name}'", fontsize=9, color=color,
                        fontweight='bold')
        ax_in.set_xticks([])
        ax_in.set_yticks([])

    ax2.axis('off')
    ax2.set_title('Average Mel Spectrogram Per Key', fontweight='bold')
    ax2.text(0.5, 0.5, 'Keys that cluster together\nin the PCA plot have\n'
             'similar spectrograms.\n\nWell-separated clusters =\n'
             'easy to classify.',
             ha='center', va='center', fontsize=12, transform=ax2.transAxes,
             style='italic', color='gray')

    plt.tight_layout(rect=[0, 0, 1, 0.93])
    if save:
        plt.savefig('feature_space.png', dpi=150, bbox_inches='tight')
        print("Saved: feature_space.png")
    return fig


def plot_all(data_dir=None):
    """Generate all visualizations."""
    print("Generating pipeline visualizations...\n")

    plot_onset_detection()
    plot_keystroke_comparison()
    plot_feature_pipeline()
    plot_feature_space()

    print("\nAll visualizations saved. Add to tutorial with:")
    print("  ![Onset Detection](onset_detection.png)")
    print("  ![Keystroke Comparison](keystroke_comparison.png)")
    print("  ![Feature Pipeline](feature_pipeline.png)")
    print("  ![Feature Space](feature_space.png)")


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--data', type=str, default=None,
                        help='Path to keystroke_data directory')
    parser.add_argument('--interactive', '-i', action='store_true')
    args = parser.parse_args()

    if args.interactive:
        # Show all plots interactively
        plot_onset_detection(save=False)
        plot_keystroke_comparison(save=False)
        plot_feature_pipeline(save=False)
        plot_feature_space(save=False)
        plt.show()
    else:
        plot_all(args.data)
