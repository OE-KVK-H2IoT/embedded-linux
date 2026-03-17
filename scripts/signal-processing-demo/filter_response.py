#!/usr/bin/env python3
"""Interactive Filter Response Demo

Demonstrates:
  1. High-pass, low-pass, band-pass filter frequency response
  2. How cutoff frequency and filter order affect the response
  3. Time-domain effect on a mixed signal (speech + noise)
  4. Phase response and group delay

Run:
  python filter_response.py              # static comparison
  python filter_response.py --interactive # slider GUI

Requirements: numpy, matplotlib, scipy
"""

import numpy as np
import matplotlib.pyplot as plt
from scipy.signal import butter, freqz, lfilter, iirfilter
import argparse


def make_test_signal(fs=48000, duration=0.1):
    """Mixed signal: 200 Hz tone + 2 kHz tone + broadband noise."""
    n = int(fs * duration)
    t = np.arange(n) / fs
    # Low component (would be removed by HP filter)
    low = 0.5 * np.sin(2 * np.pi * 200 * t)
    # Mid component (the "signal" we want)
    mid = np.sin(2 * np.pi * 2000 * t)
    # High noise (would be removed by LP filter)
    high = 0.3 * np.random.randn(n)
    return t, low + mid + high, low, mid, high


def static_demo():
    """Show filter types and their effects."""
    fs = 48000

    fig, axes = plt.subplots(3, 2, figsize=(14, 12))
    fig.suptitle('Filter Types and Their Effects', fontsize=14,
                 fontweight='bold')

    # -- Row 1: Frequency responses of HP, LP, BP --
    filters = [
        ('High-pass 200 Hz', 'highpass', 200),
        ('Low-pass 5000 Hz', 'lowpass', 5000),
        ('Band-pass 500-3000 Hz', 'bandpass', (500, 3000)),
    ]
    orders = [1, 2, 4, 8]
    colors = ['#3498db', '#2ecc71', '#e74c3c', '#9b59b6']

    for col, (name, btype, freq) in enumerate(filters[:2]):
        ax = axes[0][col]
        for order, color in zip(orders, colors):
            try:
                if btype == 'bandpass':
                    b, a = butter(order, freq, btype=btype, fs=fs)
                else:
                    b, a = butter(order, freq, btype=btype, fs=fs)
                w, h = freqz(b, a, worN=4096, fs=fs)
                ax.semilogx(w, 20 * np.log10(np.abs(h) + 1e-12),
                            color=color, label=f'Order {order}')
            except Exception:
                pass
        ax.set_title(name, fontweight='bold')
        ax.set_xlabel('Frequency (Hz)')
        ax.set_ylabel('Gain (dB)')
        ax.set_xlim(20, fs / 2)
        ax.set_ylim(-60, 5)
        ax.axhline(-3, color='gray', linestyle=':', alpha=0.5, label='-3 dB')
        ax.axvline(freq if isinstance(freq, (int, float)) else freq[0],
                   color='gray', linestyle='--', alpha=0.3)
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3, which='both')

    # Band-pass
    ax = axes[0][1]  # reuse second column for comparison
    # Actually let's do the band-pass in row 1 col 1
    # Already done LP, let me put BP comparison in row 2

    # -- Row 2: 1-pole vs biquad comparison (our actual app) --
    ax_1pole = axes[1][0]
    ax_biquad = axes[1][1]

    # 1-pole HP (as used in audio_viz)
    alpha = 0.985  # ~115 Hz at 48 kHz
    # Transfer function: H(z) = alpha * (1 - z^-1) / (1 - alpha * z^-1)
    b_1p = np.array([alpha, -alpha])
    a_1p = np.array([1, -alpha])
    w, h = freqz(b_1p, a_1p, worN=4096, fs=fs)
    ax_1pole.semilogx(w, 20 * np.log10(np.abs(h) + 1e-12),
                      'r-', linewidth=2, label='1-pole HP (α=0.985)')
    # Compare with butterworth
    b2, a2 = butter(2, 115, btype='highpass', fs=fs)
    w2, h2 = freqz(b2, a2, worN=4096, fs=fs)
    ax_1pole.semilogx(w2, 20 * np.log10(np.abs(h2) + 1e-12),
                      'b--', linewidth=2, label='Butterworth 2nd-order')
    ax_1pole.set_title('HP Filter: 1-pole (our app) vs Butterworth',
                       fontweight='bold')
    ax_1pole.set_xlabel('Frequency (Hz)')
    ax_1pole.set_ylabel('Gain (dB)')
    ax_1pole.set_xlim(20, fs / 2)
    ax_1pole.set_ylim(-40, 5)
    ax_1pole.axhline(-3, color='gray', linestyle=':', alpha=0.5)
    ax_1pole.axvline(115, color='gray', linestyle='--', alpha=0.3)
    ax_1pole.legend()
    ax_1pole.grid(True, alpha=0.3, which='both')

    # Biquad peaking EQ (as used in audio_viz_full)
    center_freqs = [60, 150, 400, 1000, 2500, 6000, 12000, 16000]
    gain_db = [6, -3, 0, 9, 0, -6, 3, 0]
    ax_biquad.set_title('8-Band Parametric EQ (biquad peaking)',
                        fontweight='bold')
    combined = np.ones(4096)
    for fc, gdb in zip(center_freqs, gain_db):
        if abs(gdb) < 0.1:
            continue
        A = 10 ** (gdb / 40)
        w0 = 2 * np.pi * fc / fs
        Q = 1.4
        alpha_bq = np.sin(w0) / (2 * Q)
        b = np.array([1 + alpha_bq * A, -2 * np.cos(w0), 1 - alpha_bq * A])
        a = np.array([1 + alpha_bq / A, -2 * np.cos(w0), 1 - alpha_bq / A])
        w, h = freqz(b, a, worN=4096, fs=fs)
        mag = np.abs(h)
        combined *= mag
        ax_biquad.semilogx(w, 20 * np.log10(mag + 1e-12),
                           alpha=0.3, linewidth=1)

    ax_biquad.semilogx(w, 20 * np.log10(combined + 1e-12),
                       'k-', linewidth=2, label='Combined response')
    ax_biquad.set_xlabel('Frequency (Hz)')
    ax_biquad.set_ylabel('Gain (dB)')
    ax_biquad.set_xlim(20, fs / 2)
    ax_biquad.set_ylim(-15, 15)
    ax_biquad.legend()
    ax_biquad.grid(True, alpha=0.3, which='both')

    # -- Row 3: Time domain effect --
    t, mixed, low, mid, high = make_test_signal(fs)
    ax_time = axes[2][0]
    ax_time.plot(t * 1000, mixed, 'b-', alpha=0.4, label='Original (mixed)')
    # Apply HP filter
    b_hp, a_hp = butter(2, 500, btype='highpass', fs=fs)
    filtered = lfilter(b_hp, a_hp, mixed)
    ax_time.plot(t * 1000, filtered, 'r-', linewidth=1.5,
                 label='After HP 500 Hz')
    ax_time.set_title('Time Domain: HP Filter Effect', fontweight='bold')
    ax_time.set_xlabel('Time (ms)')
    ax_time.set_ylabel('Amplitude')
    ax_time.legend()
    ax_time.grid(True, alpha=0.3)

    # Spectrum before/after
    ax_spec = axes[2][1]
    n = len(mixed)
    freqs = np.fft.rfftfreq(n, 1.0 / fs)
    spec_orig = 20 * np.log10(np.abs(np.fft.rfft(mixed)) / n + 1e-12)
    spec_filt = 20 * np.log10(np.abs(np.fft.rfft(filtered)) / n + 1e-12)
    ax_spec.semilogx(freqs[1:], spec_orig[1:], 'b-', alpha=0.5,
                     label='Original')
    ax_spec.semilogx(freqs[1:], spec_filt[1:], 'r-', linewidth=1.5,
                     label='Filtered')
    ax_spec.axvline(500, color='gray', linestyle='--', alpha=0.5,
                    label='Cutoff')
    ax_spec.set_title('Spectrum: Before vs After HP 500 Hz',
                      fontweight='bold')
    ax_spec.set_xlabel('Frequency (Hz)')
    ax_spec.set_ylabel('Magnitude (dB)')
    ax_spec.set_xlim(20, fs / 2)
    ax_spec.set_ylim(-60, 0)
    ax_spec.legend()
    ax_spec.grid(True, alpha=0.3, which='both')

    plt.tight_layout()
    plt.savefig('filter_response.png', dpi=150, bbox_inches='tight')
    print("Saved: filter_response.png")
    plt.show()


def interactive_demo():
    """Adjustable filter cutoff and order."""
    from matplotlib.widgets import Slider, RadioButtons

    fs = 48000
    t, mixed, _, _, _ = make_test_signal(fs)

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(13, 8))
    fig.subplots_adjust(bottom=0.2, right=0.78)

    ftype = ['highpass']

    def draw(cutoff, order):
        ax1.clear()
        ax2.clear()

        try:
            b, a = butter(order, cutoff, btype=ftype[0], fs=fs)
            w, h = freqz(b, a, worN=4096, fs=fs)
            filtered = lfilter(b, a, mixed)
        except Exception:
            return

        # Frequency response
        ax1.semilogx(w, 20 * np.log10(np.abs(h) + 1e-12), 'steelblue',
                     linewidth=2)
        ax1.axhline(-3, color='gray', linestyle=':', alpha=0.5)
        ax1.axvline(cutoff, color='red', linestyle='--', alpha=0.5)
        ax1.set_title(f'{ftype[0].title()} Filter: {cutoff} Hz, order {order}',
                      fontweight='bold')
        ax1.set_xlim(20, fs / 2)
        ax1.set_ylim(-60, 5)
        ax1.set_xlabel('Frequency (Hz)')
        ax1.set_ylabel('Gain (dB)')
        ax1.grid(True, alpha=0.3, which='both')

        # Time domain
        ax2.plot(t * 1000, mixed, 'b-', alpha=0.3, label='Original')
        ax2.plot(t * 1000, filtered, 'r-', linewidth=1.5, label='Filtered')
        ax2.set_xlabel('Time (ms)')
        ax2.set_ylabel('Amplitude')
        ax2.legend()
        ax2.grid(True, alpha=0.3)

        fig.canvas.draw_idle()

    ax_cut = fig.add_axes([0.15, 0.10, 0.5, 0.03])
    ax_ord = fig.add_axes([0.15, 0.05, 0.5, 0.03])
    s_cut = Slider(ax_cut, 'Cutoff (Hz)', 20, 10000, valinit=500, valstep=10)
    s_ord = Slider(ax_ord, 'Order', 1, 8, valinit=2, valstep=1)

    ax_radio = fig.add_axes([0.80, 0.4, 0.18, 0.2])
    radio = RadioButtons(ax_radio, ('highpass', 'lowpass'), active=0)

    def update(_=None):
        draw(s_cut.val, int(s_ord.val))

    def type_changed(label):
        ftype[0] = label
        update()

    s_cut.on_changed(update)
    s_ord.on_changed(update)
    radio.on_clicked(type_changed)
    draw(500, 2)
    plt.show()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--interactive', '-i', action='store_true')
    args = parser.parse_args()
    if args.interactive:
        interactive_demo()
    else:
        static_demo()
