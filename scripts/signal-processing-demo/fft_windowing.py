#!/usr/bin/env python3
"""Interactive FFT & Windowing Demo

Demonstrates:
  1. Why windowing matters (spectral leakage)
  2. How window choice affects frequency resolution
  3. The tradeoff: main lobe width vs side lobe level
  4. Zero-padding for interpolated frequency display

Run:
  python fft_windowing.py              # static comparison
  python fft_windowing.py --interactive # slider GUI

Requirements: numpy, matplotlib
"""

import numpy as np
import matplotlib.pyplot as plt
import argparse


def generate_test_signal(freq1=440, freq2=466, amplitude2=0.1,
                         fs=48000, duration=0.05):
    """Two sine waves: one strong, one weak and close in frequency.
    The weak tone tests whether windowing reveals it or hides it."""
    n = int(fs * duration)
    t = np.arange(n) / fs
    signal = np.sin(2 * np.pi * freq1 * t) + \
             amplitude2 * np.sin(2 * np.pi * freq2 * t)
    return t, signal


def apply_window_and_fft(signal, window_name, fs):
    """Apply window, compute FFT, return magnitude in dB."""
    n = len(signal)
    windows = {
        'Rectangle': np.ones(n),
        'Hann': np.hanning(n),
        'Hamming': np.hamming(n),
        'Blackman': np.blackman(n),
        'Flat-top': (1 - 1.93 * np.cos(2*np.pi*np.arange(n)/(n-1))
                     + 1.29 * np.cos(4*np.pi*np.arange(n)/(n-1))
                     - 0.388 * np.cos(6*np.pi*np.arange(n)/(n-1))
                     + 0.032 * np.cos(8*np.pi*np.arange(n)/(n-1))),
    }
    w = windows.get(window_name, np.ones(n))
    windowed = signal * w

    # Zero-pad for smoother display (4x)
    nfft = n * 4
    fft_mag = np.abs(np.fft.rfft(windowed, n=nfft)) / n
    fft_db = 20 * np.log10(fft_mag + 1e-12)
    freqs = np.fft.rfftfreq(nfft, 1.0 / fs)

    return freqs, fft_db, w


def static_demo():
    """Compare all windows on the same signal."""
    t, signal = generate_test_signal()
    fs = 48000
    window_names = ['Rectangle', 'Hann', 'Hamming', 'Blackman', 'Flat-top']
    colors = ['#e74c3c', '#2ecc71', '#3498db', '#9b59b6', '#f39c12']

    fig, axes = plt.subplots(3, 1, figsize=(13, 10))

    # Top: signal + windows
    ax_sig = axes[0]
    n = len(signal)
    for name, color in zip(window_names, colors):
        _, _, w = apply_window_and_fft(signal, name, fs)
        ax_sig.plot(np.arange(n) / fs * 1000, w, color=color,
                    label=name, alpha=0.7)
    ax_sig.set_title('Window Functions (time domain)', fontweight='bold')
    ax_sig.set_xlabel('Time (ms)')
    ax_sig.set_ylabel('Amplitude')
    ax_sig.legend(ncol=5, fontsize=9)
    ax_sig.grid(True, alpha=0.3)

    # Middle: full spectrum
    ax_full = axes[1]
    for name, color in zip(window_names, colors):
        freqs, fft_db, _ = apply_window_and_fft(signal, name, fs)
        ax_full.plot(freqs, fft_db, color=color, label=name, alpha=0.8)
    ax_full.set_title('Spectrum: 440 Hz (strong) + 466 Hz (weak, -20 dB)\n'
                      'Can you see the weak tone?', fontweight='bold')
    ax_full.set_xlim(0, 2000)
    ax_full.set_ylim(-80, 5)
    ax_full.set_xlabel('Frequency (Hz)')
    ax_full.set_ylabel('Magnitude (dB)')
    ax_full.axvline(440, color='gray', linestyle=':', alpha=0.5)
    ax_full.axvline(466, color='gray', linestyle=':', alpha=0.5)
    ax_full.legend(ncol=5, fontsize=9)
    ax_full.grid(True, alpha=0.3)

    # Bottom: zoomed around the peaks
    ax_zoom = axes[2]
    for name, color in zip(window_names, colors):
        freqs, fft_db, _ = apply_window_and_fft(signal, name, fs)
        ax_zoom.plot(freqs, fft_db, color=color, label=name, alpha=0.8)
    ax_zoom.set_title('Zoomed: 380–520 Hz — Spectral Leakage Comparison',
                      fontweight='bold')
    ax_zoom.set_xlim(380, 520)
    ax_zoom.set_ylim(-80, 5)
    ax_zoom.set_xlabel('Frequency (Hz)')
    ax_zoom.set_ylabel('Magnitude (dB)')
    ax_zoom.axvline(440, color='gray', linestyle=':', alpha=0.5, label='440 Hz')
    ax_zoom.axvline(466, color='gray', linestyle=':', alpha=0.5, label='466 Hz')
    ax_zoom.legend(ncol=4, fontsize=8)
    ax_zoom.grid(True, alpha=0.3)

    # Add annotation
    ax_zoom.annotate('Rectangular leakage\nhides the weak tone',
                     xy=(453, -25), fontsize=9, color='#e74c3c',
                     ha='center')

    plt.tight_layout()
    plt.savefig('fft_windowing.png', dpi=150, bbox_inches='tight')
    print("Saved: fft_windowing.png")

    # Print comparison table
    print("\nWindow Comparison:")
    print(f"{'Window':<12} {'Main lobe':<14} {'Side lobe':<14} {'Best for'}")
    print("-" * 60)
    info = [
        ('Rectangle', '1 bin', '-13 dB', 'Exact bin-aligned frequencies'),
        ('Hann', '2 bins', '-31 dB', 'General purpose (our app)'),
        ('Hamming', '2 bins', '-42 dB', 'Speech processing'),
        ('Blackman', '3 bins', '-58 dB', 'High dynamic range'),
        ('Flat-top', '5 bins', '-93 dB', 'Amplitude accuracy'),
    ]
    for name, ml, sl, use in info:
        print(f'{name:<12} {ml:<14} {sl:<14} {use}')

    plt.show()


def interactive_demo():
    """Slider to adjust signal frequency and window type."""
    from matplotlib.widgets import Slider, RadioButtons

    fs = 48000
    duration = 0.05
    window_name = ['Hann']

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(13, 8))
    fig.subplots_adjust(bottom=0.2, right=0.78)

    def draw(freq1, freq2, amp2):
        n = int(fs * duration)
        t = np.arange(n) / fs
        signal = np.sin(2*np.pi*freq1*t) + amp2*np.sin(2*np.pi*freq2*t)

        freqs, fft_db, w = apply_window_and_fft(signal, window_name[0], fs)

        ax1.clear()
        ax1.plot(t * 1000, signal, 'b-', alpha=0.5, label='Signal')
        ax1.plot(t * 1000, signal * w, 'r-', label=f'Windowed ({window_name[0]})')
        ax1.set_xlabel('Time (ms)')
        ax1.set_ylabel('Amplitude')
        ax1.legend()
        ax1.grid(True, alpha=0.3)
        ax1.set_title(f'Signal: {freq1} Hz + {freq2} Hz ({20*np.log10(amp2+1e-10):.0f} dB)')

        ax2.clear()
        ax2.plot(freqs, fft_db, 'steelblue')
        ax2.axvline(freq1, color='green', linestyle='--', alpha=0.5)
        ax2.axvline(freq2, color='red', linestyle='--', alpha=0.5)
        ax2.set_xlim(0, 2000)
        ax2.set_ylim(-80, 5)
        ax2.set_xlabel('Frequency (Hz)')
        ax2.set_ylabel('Magnitude (dB)')
        ax2.set_title(f'Spectrum ({window_name[0]} window)')
        ax2.grid(True, alpha=0.3)

        fig.canvas.draw_idle()

    # Sliders
    ax_f1 = fig.add_axes([0.15, 0.10, 0.5, 0.03])
    ax_f2 = fig.add_axes([0.15, 0.05, 0.5, 0.03])
    s_f1 = Slider(ax_f1, 'Freq 1 (Hz)', 100, 2000, valinit=440, valstep=10)
    s_f2 = Slider(ax_f2, 'Freq 2 (Hz)', 100, 2000, valinit=466, valstep=10)

    # Radio buttons for window
    ax_radio = fig.add_axes([0.80, 0.3, 0.18, 0.3])
    radio = RadioButtons(ax_radio,
                         ('Rectangle', 'Hann', 'Hamming', 'Blackman'),
                         active=1)

    def update(_=None):
        draw(s_f1.val, s_f2.val, 0.1)

    def window_changed(label):
        window_name[0] = label
        update()

    s_f1.on_changed(update)
    s_f2.on_changed(update)
    radio.on_clicked(window_changed)
    draw(440, 466, 0.1)
    plt.show()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--interactive', '-i', action='store_true')
    args = parser.parse_args()
    if args.interactive:
        interactive_demo()
    else:
        static_demo()
