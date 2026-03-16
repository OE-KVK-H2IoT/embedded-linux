#!/usr/bin/env python3
"""Typing practice app that collects keystroke audio for training.

Presents words/sentences to type. Records audio continuously while
capturing exact key-press timestamps via terminal raw input.
This gives naturally-typed, properly-labeled training data.

Usage:
    python typing_practice.py                     # default settings
    python typing_practice.py --device 4          # specify audio device
    python typing_practice.py --rounds 5          # number of rounds
    python typing_practice.py --wordlist my.txt   # custom word list
"""

import argparse
import sys
import os
import time
import tty
import termios
import threading
import numpy as np
import sounddevice as sd
from pathlib import Path

RATE = 48000
BLOCK = 256

# ── Word lists for practice ─────────────────────────────────────────

PANGRAMS = [
    "the quick brown fox jumps over the lazy dog",
    "pack my box with five dozen liquor jugs",
    "how vexingly quick daft zebras jump",
    "the five boxing wizards jump quickly",
    "sphinx of black quartz judge my vow",
]

# Common English words — covers all letters with natural frequency
COMMON_WORDS = [
    "hello world", "python code", "signal data", "quick test",
    "embedded linux", "kernel driver", "sensor value",
    "frequency spectrum", "audio buffer", "real time",
    "raspberry pi", "micro controller", "device tree",
    "keyboard input", "capture sound", "analyze waveform",
    "digital filter", "sampling rate", "threshold detect",
    "machine learning", "training model", "classification",
]

# Per-key focused drills — groups of words heavy on specific keys
KEY_DRILLS = {
    'left_home':  ["fast add sad gas dad fads saga",
                   "seed fees deed greed self fed"],
    'right_home': ["jolt hook lion pink kill joy",
                   "pool look link join silk oil"],
    'top_row':    ["write your type quit power Europe",
                   "poetry equity require tower trip"],
    'bottom_row': ["box van calm zinc exam back",
                   "maze cave next club move vex"],
    'space_flow': ["a b c d e f g h i j k l m",
                   "the a is it on at to in of"],
}


def get_practice_texts(rounds, wordlist_path=None):
    """Build a list of practice texts."""
    texts = []

    if wordlist_path:
        with open(wordlist_path) as f:
            for line in f:
                line = line.strip().lower()
                if line:
                    texts.append(line)
        return texts[:rounds] if rounds else texts

    # Mix pangrams, common words, and drills
    for i in range(rounds):
        phase = i % 4
        if phase == 0:
            texts.append(PANGRAMS[i % len(PANGRAMS)])
        elif phase == 1:
            # Pick 3-4 common word phrases
            idx = (i * 3) % len(COMMON_WORDS)
            batch = [COMMON_WORDS[(idx + j) % len(COMMON_WORDS)]
                     for j in range(3)]
            texts.append(" ".join(batch))
        elif phase == 2:
            keys = list(KEY_DRILLS.keys())
            drills = KEY_DRILLS[keys[i % len(keys)]]
            texts.append(drills[i % len(drills)])
        else:
            texts.append(PANGRAMS[(i * 7) % len(PANGRAMS)])

    return texts


# ── Terminal raw input ──────────────────────────────────────────────

class RawInput:
    """Read single keypresses without waiting for Enter."""

    def __init__(self):
        self.fd = sys.stdin.fileno()
        self.old_settings = termios.tcgetattr(self.fd)

    def __enter__(self):
        tty.setraw(self.fd)
        return self

    def __exit__(self, *args):
        termios.tcsetattr(self.fd, termios.TCSADRAIN, self.old_settings)

    def read_key(self):
        """Read a single character. Returns None on Ctrl+C."""
        ch = sys.stdin.read(1)
        if ch == '\x03':  # Ctrl+C
            return None
        return ch


# ── Audio recorder ──────────────────────────────────────────────────

class AudioRecorder:
    """Record audio in background, track sample position."""

    def __init__(self, device, rate):
        self.device = device
        self.rate = rate
        self.chunks = []
        self.sample_pos = 0
        self.lock = threading.Lock()
        self.stream = None

    def _callback(self, indata, frames, time_info, status):
        with self.lock:
            self.chunks.append(indata[:, 0].copy())
            self.sample_pos += len(indata)

    def start(self):
        self.chunks = []
        self.sample_pos = 0
        self.stream = sd.InputStream(
            device=self.device, channels=1, samplerate=self.rate,
            blocksize=BLOCK, dtype='float32', callback=self._callback)
        self.stream.start()

    def current_sample(self):
        with self.lock:
            return self.sample_pos

    def stop(self):
        if self.stream:
            self.stream.stop()
            self.stream.close()
        with self.lock:
            if self.chunks:
                return np.concatenate(self.chunks)
            return np.array([], dtype=np.float32)


# ── Display helpers ─────────────────────────────────────────────────

def clear_line():
    sys.stdout.write('\r\033[K')

def write_raw(text):
    """Write to stdout in raw mode (need \r\n for newlines)."""
    sys.stdout.write(text)
    sys.stdout.flush()

def show_progress(target, typed, correct_mask):
    """Show the target text with colored feedback."""
    clear_line()
    out = []
    for i, ch in enumerate(target):
        if i < len(typed):
            if correct_mask[i]:
                out.append(f'\033[32m{ch}\033[0m')  # green
            else:
                out.append(f'\033[31m{ch}\033[0m')  # red
        elif i == len(typed):
            out.append(f'\033[4m{ch}\033[0m')  # underline = cursor
        else:
            out.append(f'\033[90m{ch}\033[0m')  # dim
    write_raw('\r' + ''.join(out))


# ── Main ────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Typing practice with keystroke audio collection")
    parser.add_argument('--output', default='keystroke_data',
                        help='Output directory (default: keystroke_data)')
    parser.add_argument('--device', default=None,
                        help='Audio input device')
    parser.add_argument('--rounds', type=int, default=8,
                        help='Number of practice rounds (default: 8)')
    parser.add_argument('--wordlist', default=None,
                        help='Custom text file (one phrase per line)')
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

    out_dir = Path(args.output)
    out_dir.mkdir(exist_ok=True)

    texts = get_practice_texts(args.rounds, args.wordlist)
    recorder = AudioRecorder(device, RATE)

    # Load existing labels if appending
    labels_path = out_dir / 'labels.npz'
    all_events = []

    print("╔══════════════════════════════════════════════════╗")
    print("║        Typing Practice — Audio Collector         ║")
    print("╠══════════════════════════════════════════════════╣")
    print("║  Type each phrase as shown.                      ║")
    print("║  Audio is recorded + keystrokes are timestamped. ║")
    print("║  Press Ctrl+C to quit anytime.                   ║")
    print("╚══════════════════════════════════════════════════╝")
    print()
    print(f"  Output:  {out_dir}/")
    print(f"  Device:  {device or 'default'}")
    print(f"  Rounds:  {len(texts)}")
    print()
    input("  Press Enter to start...")
    print()

    total_keys = 0

    for round_num, target in enumerate(texts):
        target = target.lower()
        print(f"\033[1mRound {round_num + 1}/{len(texts)}\033[0m")
        write_raw(f'  \033[90m{target}\033[0m')
        write_raw('\r\n  ')

        # Start recording
        recorder.start()
        # Small delay for stream to stabilize
        time.sleep(0.1)

        typed = []
        correct = []
        events = []

        with RawInput() as raw:
            while len(typed) < len(target):
                show_progress(target, typed, correct)

                ch = raw.read_key()
                if ch is None:  # Ctrl+C
                    audio = recorder.stop()
                    write_raw('\r\n')
                    _save_round(out_dir, round_num, audio, events)
                    all_events.extend(events)
                    _save_labels(out_dir, all_events, total_keys)
                    print(f"\r\n\033[1mStopped. Collected {total_keys} "
                          f"keystrokes total.\033[0m")
                    return

                ts = recorder.current_sample()
                expected = target[len(typed)]

                # Map enter/return → ignore, backspace → skip
                if ch in ('\r', '\n'):
                    continue
                if ch == '\x7f':  # backspace
                    if typed:
                        typed.pop()
                        correct.pop()
                        events.pop()
                    continue

                is_correct = (ch == expected)
                typed.append(ch)
                correct.append(is_correct)
                events.append({
                    'key': expected,  # always label with what SHOULD be typed
                    'typed': ch,
                    'correct': is_correct,
                    'sample': ts,
                })
                total_keys += 1

        # Done with this round
        show_progress(target, typed, correct)
        audio = recorder.stop()

        n_correct = sum(correct)
        accuracy = n_correct / len(target) * 100
        write_raw(f'\r\n  \033[90m{n_correct}/{len(target)} correct '
                  f'({accuracy:.0f}%) — {len(audio)/RATE:.1f}s audio\033[0m\r\n\r\n')

        _save_round(out_dir, round_num, audio, events)
        all_events.extend(events)

    # Save combined labels
    _save_labels(out_dir, all_events, total_keys)

    print(f"\033[1mDone! Collected {total_keys} keystrokes "
          f"across {len(texts)} rounds.\033[0m")
    print(f"Data saved to {out_dir}/")
    print(f"\nTo train: python train_model.py {out_dir}")


def _save_round(out_dir, round_num, audio, events):
    """Save audio and labels for one round."""
    np.save(out_dir / f'round_{round_num:03d}.npy', audio)


def _save_labels(out_dir, all_events, total):
    """Save all keystroke labels with timestamps."""
    if not all_events:
        return

    keys = [e['key'] for e in all_events]
    samples = [e['sample'] for e in all_events]
    correct = [e['correct'] for e in all_events]

    np.savez(out_dir / 'labels.npz',
             keys=np.array(keys),
             samples=np.array(samples, dtype=np.int64),
             correct=np.array(correct, dtype=bool))

    # Also per-key counts
    from collections import Counter
    counts = Counter(k for k, c in zip(keys, correct) if c)
    summary = sorted(counts.items())
    with open(out_dir / 'summary.txt', 'w') as f:
        f.write(f"Total keystrokes: {total}\n")
        f.write(f"Correctly typed:  {sum(correct)}\n\n")
        for key, count in summary:
            label = key if key != ' ' else 'space'
            f.write(f"  {label:>5s}: {count:3d}\n")


if __name__ == "__main__":
    main()
