#!/usr/bin/env python3
"""Interactive Sampling & Aliasing Demo

Demonstrates:
  1. How sampling rate affects signal reconstruction
  2. Nyquist theorem: must sample at ≥ 2× signal frequency
  3. Aliasing: what happens when you undersample

Run:
  python sampling_aliasing.py              # static plots
  python sampling_aliasing.py --interactive # slider-based GUI

Requirements: numpy, matplotlib
"""

import numpy as np
import matplotlib.pyplot as plt
import argparse

# %% Signal parameters
DURATION = 0.05   # 50 ms of signal
SIGNAL_FREQ = 440  # Hz (A4 note)


def make_signal(freq, duration, fs_continuous=100000):
    """Generate a 'continuous' signal (very high sample rate)."""
    t = np.arange(0, duration, 1.0 / fs_continuous)
    signal = np.sin(2 * np.pi * freq * t)
    return t, signal


def sample_signal(t_cont, sig_cont, fs):
    """Sample the continuous signal at fs Hz."""
    t_sampled = np.arange(0, t_cont[-1], 1.0 / fs)
    indices = (t_sampled * 100000).astype(int)
    indices = np.clip(indices, 0, len(sig_cont) - 1)
    return t_sampled, sig_cont[indices]


def reconstruct(t_sampled, samples, t_continuous):
    """Sinc interpolation (ideal reconstruction)."""
    reconstructed = np.zeros_like(t_continuous)
    T = t_sampled[1] - t_sampled[0] if len(t_sampled) > 1 else 1
    for i, (ts, s) in enumerate(zip(t_sampled, samples)):
        sinc = np.sinc((t_continuous - ts) / T)
        reconstructed += s * sinc
    return reconstructed


# %% Static demo
def static_demo():
    """Show sampling at different rates side by side."""
    t_cont, sig = make_signal(SIGNAL_FREQ, DURATION)

    sample_rates = [2000, 1000, 700, 440]  # last one = Nyquist violation
    fig, axes = plt.subplots(len(sample_rates), 1, figsize=(12, 10),
                             sharex=True)
    fig.suptitle(f'Sampling a {SIGNAL_FREQ} Hz Sine Wave\n'
                 f'(Nyquist: need ≥ {SIGNAL_FREQ * 2} Hz)',
                 fontsize=14, fontweight='bold')

    for ax, fs in zip(axes, sample_rates):
        t_s, samples = sample_signal(t_cont, sig, fs)
        recon = reconstruct(t_s, samples, t_cont)

        # Original
        ax.plot(t_cont * 1000, sig, 'b-', alpha=0.3, linewidth=1,
                label='Original')
        # Sample points
        ax.plot(t_s * 1000, samples, 'ro', markersize=6, label='Samples')
        # Reconstruction
        ax.plot(t_cont * 1000, recon, 'r-', linewidth=1.5,
                label='Reconstructed')

        ratio = fs / SIGNAL_FREQ
        status = '✓ OK' if fs >= 2 * SIGNAL_FREQ else '✗ ALIASED'
        ax.set_title(f'fs = {fs} Hz ({ratio:.1f}× signal freq) — {status}',
                     fontsize=11,
                     color='green' if fs >= 2 * SIGNAL_FREQ else 'red')
        ax.set_ylabel('Amplitude')
        ax.legend(loc='upper right', fontsize=8)
        ax.grid(True, alpha=0.3)
        ax.set_ylim(-1.5, 1.5)

    axes[-1].set_xlabel('Time (ms)')
    plt.tight_layout()
    plt.savefig('sampling_aliasing.png', dpi=150, bbox_inches='tight')
    print("Saved: sampling_aliasing.png")
    plt.show()


# %% Interactive demo with sliders
def interactive_demo():
    """Matplotlib slider to adjust sample rate in real time."""
    from matplotlib.widgets import Slider

    t_cont, sig = make_signal(SIGNAL_FREQ, DURATION)

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 7))
    fig.subplots_adjust(bottom=0.2)

    # Initial plot
    fs_init = 2000
    t_s, samples = sample_signal(t_cont, sig, fs_init)
    recon = reconstruct(t_s, samples, t_cont)

    line_orig, = ax1.plot(t_cont * 1000, sig, 'b-', alpha=0.3, label='Original')
    dots, = ax1.plot(t_s * 1000, samples, 'ro', markersize=6, label='Samples')
    line_recon, = ax1.plot(t_cont * 1000, recon, 'r-', linewidth=1.5,
                           label='Reconstructed')
    ax1.set_ylabel('Amplitude')
    ax1.legend()
    ax1.grid(True, alpha=0.3)
    ax1.set_ylim(-1.5, 1.5)
    ax1.set_title(f'Signal: {SIGNAL_FREQ} Hz | Sample rate: {fs_init} Hz')

    # Frequency domain
    freqs = np.fft.rfftfreq(len(t_s), 1.0 / fs_init)
    spectrum = np.abs(np.fft.rfft(samples)) / len(samples)
    bars = ax2.bar(freqs, spectrum, width=freqs[1] if len(freqs) > 1 else 1,
                   color='steelblue', alpha=0.7)
    nyquist_line = ax2.axvline(fs_init / 2, color='red', linestyle='--',
                                label='Nyquist')
    signal_line = ax2.axvline(SIGNAL_FREQ, color='green', linestyle='--',
                               label=f'Signal ({SIGNAL_FREQ} Hz)')
    ax2.set_xlabel('Frequency (Hz)')
    ax2.set_ylabel('Magnitude')
    ax2.set_xlim(0, 3000)
    ax2.legend()
    ax2.grid(True, alpha=0.3)

    # Slider
    ax_slider = fig.add_axes([0.15, 0.05, 0.7, 0.03])
    slider = Slider(ax_slider, 'Sample Rate (Hz)', 200, 5000,
                    valinit=fs_init, valstep=50)

    def update(val):
        fs = int(slider.val)
        t_s, samples = sample_signal(t_cont, sig, fs)
        recon = reconstruct(t_s, samples, t_cont)

        dots.set_data(t_s * 1000, samples)
        line_recon.set_ydata(recon)

        status = '✓' if fs >= 2 * SIGNAL_FREQ else '✗ ALIASED'
        color = 'green' if fs >= 2 * SIGNAL_FREQ else 'red'
        ax1.set_title(f'Signal: {SIGNAL_FREQ} Hz | Sample rate: {fs} Hz  '
                      f'{status}', color=color)

        # Update spectrum
        ax2.clear()
        freqs = np.fft.rfftfreq(len(t_s), 1.0 / fs)
        spectrum = np.abs(np.fft.rfft(samples)) / max(len(samples), 1)
        ax2.bar(freqs, spectrum,
                width=freqs[1] if len(freqs) > 1 else 1,
                color='steelblue', alpha=0.7)
        ax2.axvline(fs / 2, color='red', linestyle='--', label='Nyquist')
        ax2.axvline(SIGNAL_FREQ, color='green', linestyle='--',
                    label=f'Signal ({SIGNAL_FREQ} Hz)')
        ax2.set_xlabel('Frequency (Hz)')
        ax2.set_ylabel('Magnitude')
        ax2.set_xlim(0, 3000)
        ax2.legend()
        ax2.grid(True, alpha=0.3)

        fig.canvas.draw_idle()

    slider.on_changed(update)
    plt.show()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--interactive', '-i', action='store_true')
    args = parser.parse_args()

    if args.interactive:
        interactive_demo()
    else:
        static_demo()
