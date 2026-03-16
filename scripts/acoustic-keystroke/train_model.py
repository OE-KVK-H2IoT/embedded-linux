#!/usr/bin/env python3
"""Train keystroke classifier from collected audio data.

Usage:
    python train_model.py                          # default keystroke_data/
    python train_model.py path/to/data_dir         # specify data directory
    python train_model.py --augment                # enable data augmentation
    python train_model.py --model svm              # use SVM instead of RF
"""

import argparse
import pickle
import numpy as np
from pathlib import Path
from sklearn.ensemble import RandomForestClassifier
from sklearn.svm import SVC
from sklearn.model_selection import cross_val_score
from sklearn.preprocessing import StandardScaler
from sklearn.pipeline import Pipeline
from sklearn.metrics import confusion_matrix, classification_report

from features import (RATE, extract_features, detect_onsets,
                       extract_keystroke_segment)


# ── Dataset builder ─────────────────────────────────────────────────

def build_dataset(data_dir, augment=False):
    """Load training data from both formats:
    - Old format: key_*.npy files (one per key, onset detection)
    - New format: round_*.npy + labels.npz (typing practice, timestamped)
    """
    data_dir = Path(data_dir)
    X, y = [], []

    # ── New format: typing practice rounds with timestamps ───────
    labels_path = data_dir / 'labels.npz'
    if labels_path.exists():
        labels = np.load(labels_path, allow_pickle=True)
        keys = labels['keys']
        samples = labels['samples']
        correct = labels['correct']

        # Load all round audio files
        round_files = sorted(data_dir.glob("round_*.npy"))
        if round_files:
            print(f"  Loading {len(round_files)} practice rounds...")
            rounds = []
            round_lengths = []
            for rf in round_files:
                audio = np.load(rf)
                rounds.append(audio)
                round_lengths.append(len(audio))

            # Concatenate all rounds into one long audio stream
            full_audio = np.concatenate(rounds)

            # Extract segments at each labeled timestamp
            n_used = 0
            n_skipped = 0
            for key, sample_pos, is_correct in zip(keys, samples, correct):
                if not is_correct:
                    n_skipped += 1
                    continue
                if sample_pos >= len(full_audio):
                    n_skipped += 1
                    continue

                segment = extract_keystroke_segment(
                    full_audio, int(sample_pos))
                features = extract_features(segment)
                label = key if key != ' ' else 'space'
                X.append(features)
                y.append(label)
                n_used += 1

                if augment:
                    for aug_seg in augment_segment(segment):
                        X.append(extract_features(aug_seg))
                        y.append(label)

            print(f"  Practice data: {n_used} keystrokes used, "
                  f"{n_skipped} skipped (mistyped/oob)")

    # ── Old format: per-key recordings with onset detection ──────
    key_files = sorted(data_dir.glob("key_*.npy"))
    if key_files:
        print(f"  Loading {len(key_files)} per-key recordings...")
        for npy_file in key_files:
            label = npy_file.stem.replace("key_", "")
            audio = np.load(npy_file)

            onsets = detect_onsets(audio, RATE)
            print(f"    {label}: {len(onsets)} keystrokes detected")

            for onset in onsets:
                segment = extract_keystroke_segment(audio, onset)
                features = extract_features(segment)
                X.append(features)
                y.append(label)

                if augment:
                    for aug_seg in augment_segment(segment):
                        X.append(extract_features(aug_seg))
                        y.append(label)

    return np.array(X), np.array(y)


def augment_segment(segment):
    """Create augmented copies of a keystroke segment."""
    augmented = []
    rate = RATE

    # Time shift: +/- 3ms
    shift = int(rate * 0.003)
    augmented.append(np.roll(segment, shift))
    augmented.append(np.roll(segment, -shift))

    # Add slight noise
    noise = np.random.randn(len(segment)) * 0.002
    augmented.append(segment + noise)

    # Slight gain variation (+/- 15%)
    augmented.append(segment * 1.15)
    augmented.append(segment * 0.85)

    return augmented


# ── Main ────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Train keystroke classifier")
    parser.add_argument('data_dir', nargs='?', default='keystroke_data',
                        help='Training data directory')
    parser.add_argument('--model', choices=['rf', 'svm'], default='svm',
                        help='Classifier: rf (Random Forest) or svm (default)')
    parser.add_argument('--augment', action='store_true',
                        help='Enable data augmentation (5x more samples)')
    parser.add_argument('--output', default='keystroke_model.pkl',
                        help='Output model path')
    args = parser.parse_args()

    print(f"Loading data from {args.data_dir}/ ...")
    X, y = build_dataset(args.data_dir, augment=args.augment)
    print(f"Dataset: {len(X)} samples, {len(set(y))} classes, "
          f"{X.shape[1]} features\n")

    # Build pipeline
    if args.model == 'svm':
        clf = SVC(kernel='rbf', C=10, gamma='scale', probability=True,
                  random_state=42)
        name = "SVM (RBF)"
    else:
        clf = RandomForestClassifier(n_estimators=200, random_state=42,
                                     max_depth=None, min_samples_leaf=1)
        name = "Random Forest"

    pipeline = Pipeline([
        ("scaler", StandardScaler()),
        ("clf", clf)
    ])

    # Cross-validation
    scores = cross_val_score(pipeline, X, y, cv=5, scoring="accuracy")
    print(f"{name} cross-validation: {scores.mean():.1%} "
          f"(+/- {scores.std():.1%})")

    # Train final model on all data
    pipeline.fit(X, y)

    # Show confusion info
    y_pred = pipeline.predict(X)
    print(f"\nTraining accuracy: {np.mean(y_pred == y):.1%}")

    # Find most confused pairs
    labels = sorted(set(y))
    cm = confusion_matrix(y, y_pred, labels=labels)
    np.fill_diagonal(cm, 0)
    worst = []
    for i in range(len(labels)):
        for j in range(len(labels)):
            if cm[i, j] > 0:
                worst.append((cm[i, j], labels[i], labels[j]))
    worst.sort(reverse=True)
    if worst:
        print("\nMost confused pairs (true → predicted):")
        for count, true, pred in worst[:5]:
            print(f"  {true} → {pred}: {count}x")

    # Save
    with open(args.output, "wb") as f:
        pickle.dump(pipeline, f)
    print(f"\nModel saved to {args.output}")


if __name__ == "__main__":
    main()
