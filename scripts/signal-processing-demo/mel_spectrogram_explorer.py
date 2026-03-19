#!/usr/bin/env python3
"""Interactive Mel Spectrogram Explorer

Demonstrates:
  1. How a mel spectrogram is built step by step
  2. Linear vs mel frequency scale
  3. Effect of FFT size, hop size, and number of mel bands
  4. Why spectrograms work for audio classification

Generates a synthetic multi-tone signal or loads a WAV file.

Run:
  python mel_spectrogram_explorer.py                    # synthetic signal
  python mel_spectrogram_explorer.py --wav recording.wav # from WAV file
  python mel_spectrogram_explorer.py --interactive       # slider GUI

Requirements: numpy, matplotlib, scipy
"""

import numpy as np
import matplotlib.pyplot as plt
from scipy.fft import rfft, rfftfreq
import argparse


def hz_to_mel(f):
    """Convert frequency in Hz to mel scale."""
    return 2595 * np.log10(1 + f / 700)


def mel_to_hz(m):
    """Convert mel to Hz."""
    return 700 * (10 ** (m / 2595) - 1)


def make_mel_filterbank(n_mels, n_fft, fs, f_min=100, f_max=12000):
    """Create triangular mel filterbank matrix.

    KNOWN ISSUE: when n_mels is large relative to n_fft/2, upper
    mel triangles collapse to zero width (adjacent center frequencies
    map to the same FFT bin).  These bands produce zero energy →
    black lines in the spectrogram.

    This is NOT a bug — it's a fundamental constraint:
      max useful mel bands ≈ n_fft / 8 to n_fft / 4
      (e.g., FFT=512 → max ~64 useful mel bands,
       FFT=256 → max ~32, FFT=128 → max ~16)

    Fix: increase FFT size to get more frequency bins.
    """
    mel_min = hz_to_mel(f_min)
    mel_max = hz_to_mel(f_max)
    mel_points = np.linspace(mel_min, mel_max, n_mels + 2)
    hz_points = mel_to_hz(mel_points)

    bin_points = np.round(hz_points / fs * n_fft).astype(int)
    n_bins = n_fft // 2 + 1
    bin_points = np.clip(bin_points, 0, n_bins - 1)

    fb = np.zeros((n_mels, n_bins))
    n_empty = 0

    for m in range(n_mels):
        left = bin_points[m]
        center = bin_points[m + 1]
        right = bin_points[m + 2]

        # Count degenerate (zero-width) triangles
        if left == center or center == right:
            n_empty += 1

        for k in range(left, center):
            if center > left:
                fb[m, k] = (k - left) / (center - left)
        for k in range(center, right):
            if right > center:
                fb[m, k] = (right - k) / (right - center)

    if n_empty > 0:
        print(f"  Warning: {n_empty}/{n_mels} mel bands have zero-width "
              f"triangles (FFT has only {n_bins} bins for {n_mels} bands). "
              f"Increase FFT size to fix.")

    return fb, hz_points


def generate_test_signal(fs=48000, duration=0.5):
    """Multi-component signal: sweep + harmonics + transient."""
    n = int(fs * duration)
    t = np.arange(n) / fs

    # Frequency sweep 200 → 2000 Hz
    f_start, f_end = 200, 2000
    phase = 2 * np.pi * f_start * t + \
            2 * np.pi * (f_end - f_start) / (2 * duration) * t ** 2
    sweep = 0.5 * np.sin(phase)

    # Harmonic tone at 440 Hz (first half only)
    tone = np.zeros(n)
    tone[:n // 2] = 0.3 * np.sin(2 * np.pi * 440 * t[:n // 2])

    # Transient click at 0.25s
    click = np.zeros(n)
    click_pos = int(0.25 * fs)
    click_len = int(0.005 * fs)  # 5ms
    click[click_pos:click_pos + click_len] = \
        np.random.randn(click_len) * 0.8

    signal = sweep + tone + click
    return t, signal


def compute_spectrogram(signal, fs, n_fft=512, hop=128):
    """Compute STFT magnitude spectrogram."""
    n_frames = (len(signal) - n_fft) // hop + 1
    n_bins = n_fft // 2 + 1
    spec = np.zeros((n_bins, n_frames))
    window = np.hanning(n_fft)

    for i in range(n_frames):
        frame = signal[i * hop: i * hop + n_fft] * window
        fft = np.abs(rfft(frame)) ** 2
        spec[:, i] = fft

    return spec


def static_demo(wav_file=None):
    """Step-by-step spectrogram construction."""
    fs = 48000

    if wav_file:
        import wave
        with wave.open(wav_file, 'r') as wf:
            fs = wf.getframerate()
            n = wf.getnframes()
            raw = wf.readframes(n)
            if wf.getsampwidth() == 4:
                data = np.frombuffer(raw, dtype=np.int32).astype(np.float32)
                data /= 2 ** 31
            elif wf.getsampwidth() == 2:
                data = np.frombuffer(raw, dtype=np.int16).astype(np.float32)
                data /= 2 ** 15
            else:
                data = np.frombuffer(raw, dtype=np.uint8).astype(np.float32)
                data = (data - 128) / 128
            if wf.getnchannels() > 1:
                data = data[::wf.getnchannels()]  # mono
            t = np.arange(len(data)) / fs
            signal = data[:int(fs * 0.5)] if len(data) > fs * 0.5 else data
            t = t[:len(signal)]
        print(f"Loaded: {wav_file} ({fs} Hz, {len(signal)} samples)")
    else:
        t, signal = generate_test_signal(fs)

    fig = plt.figure(figsize=(16, 14))
    fig.suptitle('Building a Mel Spectrogram — Step by Step',
                 fontsize=14, fontweight='bold')

    # Step 1: Waveform
    ax1 = fig.add_subplot(3, 2, 1)
    ax1.plot(t * 1000, signal, 'b-', linewidth=0.5)
    ax1.set_title('Step 1: Raw Waveform', fontweight='bold')
    ax1.set_xlabel('Time (ms)')
    ax1.set_ylabel('Amplitude')
    ax1.grid(True, alpha=0.3)

    # Step 2: Linear spectrogram
    n_fft = 512
    hop = 128
    spec = compute_spectrogram(signal, fs, n_fft, hop)
    spec_db = 10 * np.log10(spec + 1e-10)

    ax2 = fig.add_subplot(3, 2, 2)
    t_frames = np.arange(spec.shape[1]) * hop / fs * 1000
    f_bins = rfftfreq(n_fft, 1.0 / fs)
    ax2.pcolormesh(t_frames, f_bins, spec_db, cmap='magma', shading='auto')
    ax2.set_title('Step 2: Linear Spectrogram (STFT)', fontweight='bold')
    ax2.set_xlabel('Time (ms)')
    ax2.set_ylabel('Frequency (Hz)')
    ax2.set_ylim(0, 12000)

    # Step 3: Mel scale comparison
    ax3 = fig.add_subplot(3, 2, 3)
    f_lin = np.linspace(0, 12000, 200)
    f_mel = hz_to_mel(f_lin)
    ax3.plot(f_lin, f_lin, 'b-', label='Linear scale')
    ax3.plot(f_lin, mel_to_hz(np.linspace(0, hz_to_mel(12000), 200)),
             'r-', label='Mel scale (→Hz)')
    ax3.set_title('Step 3: Linear vs Mel Frequency Scale', fontweight='bold')
    ax3.set_xlabel('Linear Frequency (Hz)')
    ax3.set_ylabel('Mapped Frequency (Hz)')
    ax3.legend()
    ax3.grid(True, alpha=0.3)
    ax3.annotate('Mel compresses\nhigh frequencies',
                 xy=(8000, 4000), fontsize=10, color='red')

    # Step 4: Mel filterbank
    n_mels = 32
    fb, hz_pts = make_mel_filterbank(n_mels, n_fft, fs)
    ax4 = fig.add_subplot(3, 2, 4)
    for m in range(n_mels):
        ax4.plot(f_bins, fb[m, :], alpha=0.5)
    ax4.set_title(f'Step 4: Mel Filterbank ({n_mels} bands)',
                  fontweight='bold')
    ax4.set_xlabel('Frequency (Hz)')
    ax4.set_ylabel('Weight')
    ax4.set_xlim(0, 12000)
    ax4.grid(True, alpha=0.3)
    ax4.annotate('Dense at low freq\n(more detail)',
                 xy=(500, 0.8), fontsize=9)
    ax4.annotate('Sparse at high freq\n(less detail)',
                 xy=(8000, 0.6), fontsize=9)

    # Step 5: Mel spectrogram
    mel_spec = fb @ spec
    mel_spec_db = 10 * np.log10(mel_spec + 1e-10)

    ax5 = fig.add_subplot(3, 2, 5)
    ax5.pcolormesh(t_frames, np.arange(n_mels), mel_spec_db,
                   cmap='magma', shading='auto')
    ax5.set_title('Step 5: Mel Spectrogram', fontweight='bold')
    ax5.set_xlabel('Time (ms)')
    ax5.set_ylabel('Mel band')

    # Step 6: As CNN input
    ax6 = fig.add_subplot(3, 2, 6)
    # Normalize to 0-1 for display
    mel_norm = (mel_spec_db - mel_spec_db.min()) / \
               (mel_spec_db.max() - mel_spec_db.min() + 1e-10)
    ax6.imshow(mel_norm, aspect='auto', origin='lower', cmap='magma')
    ax6.set_title('Step 6: Normalized — Ready for ML\n'
                  '(32 × N matrix = "image" for CNN)',
                  fontweight='bold')
    ax6.set_xlabel('Time frame')
    ax6.set_ylabel('Mel band')

    plt.tight_layout()
    plt.savefig('mel_spectrogram_steps.png', dpi=150, bbox_inches='tight')
    print("Saved: mel_spectrogram_steps.png")
    plt.show()


def interactive_demo():
    """Adjust FFT size, hop, and mel bands."""
    from matplotlib.widgets import Slider

    fs = 48000
    _, signal = generate_test_signal(fs)

    fig, axes = plt.subplots(2, 2, figsize=(14, 9))
    fig.subplots_adjust(bottom=0.2)

    def draw(n_fft, hop, n_mels):
        n_fft = int(n_fft)
        hop = int(hop)
        n_mels = int(n_mels)

        for ax in axes.flat:
            ax.clear()

        spec = compute_spectrogram(signal, fs, n_fft, hop)
        spec_db = 10 * np.log10(spec + 1e-10)
        t_fr = np.arange(spec.shape[1]) * hop / fs * 1000
        f_bins = rfftfreq(n_fft, 1.0 / fs)

        # Linear spectrogram
        axes[0][0].pcolormesh(t_fr, f_bins, spec_db, cmap='magma',
                              shading='auto')
        axes[0][0].set_title(f'Linear Spectrogram (FFT={n_fft}, hop={hop})')
        axes[0][0].set_ylabel('Frequency (Hz)')
        axes[0][0].set_ylim(0, 12000)

        # Mel filterbank
        fb, _ = make_mel_filterbank(n_mels, n_fft, fs)
        for m in range(n_mels):
            axes[0][1].plot(f_bins, fb[m, :], alpha=0.4)
        axes[0][1].set_title(f'Mel Filterbank ({n_mels} bands)')
        axes[0][1].set_xlim(0, 12000)

        # Mel spectrogram
        mel_spec = fb @ spec
        mel_db = 10 * np.log10(mel_spec + 1e-10)
        axes[1][0].pcolormesh(t_fr, np.arange(n_mels), mel_db,
                              cmap='magma', shading='auto')
        axes[1][0].set_title('Mel Spectrogram')
        axes[1][0].set_xlabel('Time (ms)')
        axes[1][0].set_ylabel('Mel band')

        # Info panel
        freq_res = fs / n_fft
        time_res = hop / fs * 1000
        n_bins = n_fft // 2 + 1
        axes[1][1].axis('off')

        # Count empty mel filters (zero-width triangles)
        n_empty = 0
        for m in range(n_mels):
            if np.sum(fb[m, :]) < 1e-10:
                n_empty += 1

        info = (f"FFT size: {n_fft} samples\n"
                f"Freq bins: {n_bins}\n"
                f"Frequency resolution: {freq_res:.1f} Hz/bin\n"
                f"Hop size: {hop} samples\n"
                f"Time resolution: {time_res:.1f} ms/frame\n"
                f"Mel bands: {n_mels}\n"
                f"Feature vector: {n_mels} × {spec.shape[1]} "
                f"= {n_mels * spec.shape[1]} values")

        if n_empty > 0:
            info += (f"\n\n⚠ {n_empty} EMPTY mel bands!\n"
                     f"Only {n_bins} FFT bins for {n_mels} bands.\n"
                     f"Upper mel triangles have zero width\n"
                     f"→ black lines in spectrogram.\n"
                     f"Fix: increase FFT size to {n_mels * 8}+")

        axes[1][1].text(0.05, 0.5, info, fontsize=11, fontfamily='monospace',
                        transform=axes[1][1].transAxes, verticalalignment='center',
                        color='red' if n_empty > 0 else 'black')
        axes[1][1].set_title('Parameters' if n_empty == 0
                             else '⚠ Parameters — resolution mismatch!')

        fig.canvas.draw_idle()

    ax_fft = fig.add_axes([0.15, 0.10, 0.3, 0.03])
    ax_hop = fig.add_axes([0.15, 0.05, 0.3, 0.03])
    ax_mel = fig.add_axes([0.55, 0.10, 0.3, 0.03])
    s_fft = Slider(ax_fft, 'FFT size', 128, 2048, valinit=512, valstep=128)
    s_hop = Slider(ax_hop, 'Hop size', 32, 512, valinit=128, valstep=32)
    s_mel = Slider(ax_mel, 'Mel bands', 8, 64, valinit=32, valstep=4)

    def update(_=None):
        draw(s_fft.val, s_hop.val, s_mel.val)

    s_fft.on_changed(update)
    s_hop.on_changed(update)
    s_mel.on_changed(update)
    draw(512, 128, 32)
    plt.show()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--interactive', '-i', action='store_true')
    parser.add_argument('--wav', type=str, default=None,
                        help='Load WAV file instead of synthetic signal')
    args = parser.parse_args()
    if args.interactive:
        interactive_demo()
    else:
        static_demo(args.wav)
