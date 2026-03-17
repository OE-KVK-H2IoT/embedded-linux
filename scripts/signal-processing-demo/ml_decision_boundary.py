#!/usr/bin/env python3
"""Interactive ML Decision Boundary Demo

Demonstrates:
  1. How SVM, Random Forest, and k-NN draw decision boundaries
  2. Effect of data quantity on model accuracy
  3. Overfitting vs underfitting visually
  4. Why feature scaling matters

Uses synthetic 2D data (2 features from mel spectrogram)
so the decision boundary can be plotted.

Run:
  python ml_decision_boundary.py              # static comparison
  python ml_decision_boundary.py --interactive # adjustable parameters

Requirements: numpy, matplotlib, scikit-learn
"""

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.colors import ListedColormap
import argparse

try:
    from sklearn.svm import SVC
    from sklearn.ensemble import RandomForestClassifier
    from sklearn.neighbors import KNeighborsClassifier
    from sklearn.preprocessing import StandardScaler
    from sklearn.model_selection import train_test_split
    from sklearn.datasets import make_classification
    HAS_SKLEARN = True
except ImportError:
    HAS_SKLEARN = False
    print("Warning: scikit-learn not installed. Install with: pip3 install scikit-learn")


def make_keystroke_data(n_samples=200, n_classes=5, separation=1.0,
                        random_state=42):
    """Generate 2D data that mimics keystroke feature distributions.
    Each class = one key, features = 2 most important mel bands."""
    X, y = make_classification(
        n_samples=n_samples,
        n_features=2,
        n_informative=2,
        n_redundant=0,
        n_classes=n_classes,
        n_clusters_per_class=1,
        class_sep=separation,
        random_state=random_state,
    )
    return X, y


def plot_decision_boundary(ax, clf, X, y, title, cmap_bg, cmap_pts):
    """Plot the classifier's decision boundary in 2D."""
    h = 0.05  # mesh resolution
    x_min, x_max = X[:, 0].min() - 1, X[:, 0].max() + 1
    y_min, y_max = X[:, 1].min() - 1, X[:, 1].max() + 1
    xx, yy = np.meshgrid(np.arange(x_min, x_max, h),
                         np.arange(y_min, y_max, h))

    Z = clf.predict(np.c_[xx.ravel(), yy.ravel()])
    Z = Z.reshape(xx.shape)

    ax.contourf(xx, yy, Z, alpha=0.3, cmap=cmap_bg)
    ax.contour(xx, yy, Z, colors='k', linewidths=0.5, alpha=0.3)
    scatter = ax.scatter(X[:, 0], X[:, 1], c=y, cmap=cmap_pts,
                         edgecolors='k', s=30, alpha=0.8)
    ax.set_title(title, fontweight='bold', fontsize=11)
    ax.set_xlabel('Mel band 5 (800 Hz)')
    ax.set_ylabel('Mel band 12 (2 kHz)')
    return scatter


def static_demo():
    """Compare three classifiers on the same data."""
    if not HAS_SKLEARN:
        print("scikit-learn required for this demo")
        return

    X, y = make_keystroke_data(n_samples=300, n_classes=5, separation=1.2)
    scaler = StandardScaler()
    X_scaled = scaler.fit_transform(X)

    classifiers = [
        ('SVM (RBF kernel)', SVC(kernel='rbf', C=10, gamma='scale')),
        ('Random Forest (100 trees)', RandomForestClassifier(n_estimators=100,
                                                              random_state=42)),
        ('k-NN (k=5)', KNeighborsClassifier(n_neighbors=5)),
    ]

    cmap_bg = ListedColormap(['#ffaaaa', '#aaffaa', '#aaaaff',
                               '#ffffaa', '#ffaaff'])
    cmap_pts = ListedColormap(['#ff0000', '#00aa00', '#0000ff',
                                '#aaaa00', '#aa00aa'])

    fig, axes = plt.subplots(1, 3, figsize=(18, 5.5))
    fig.suptitle('ML Classifiers — Decision Boundaries on Keystroke Features\n'
                 '(5 keys, 2D projection of mel spectrogram features)',
                 fontsize=13, fontweight='bold')

    for ax, (name, clf) in zip(axes, classifiers):
        clf.fit(X_scaled, y)
        score = clf.score(X_scaled, y)
        plot_decision_boundary(ax, clf, X_scaled, y,
                               f'{name}\nTrain accuracy: {score:.1%}',
                               cmap_bg, cmap_pts)

    plt.tight_layout()
    plt.savefig('ml_decision_boundary.png', dpi=150, bbox_inches='tight')
    print("Saved: ml_decision_boundary.png")
    plt.show()

    # -- Part 2: Data quantity effect --
    print("\n=== Data Quantity Experiment ===")
    fig2, axes2 = plt.subplots(2, 4, figsize=(18, 9))
    fig2.suptitle('Effect of Training Data Quantity on SVM Decision Boundary',
                  fontsize=13, fontweight='bold')

    X_full, y_full = make_keystroke_data(n_samples=1000, n_classes=5,
                                         separation=1.0)
    X_full_s = StandardScaler().fit_transform(X_full)

    for idx, n in enumerate([10, 20, 50, 100, 200, 500, 800, 1000]):
        ax = axes2.flat[idx]
        X_sub, y_sub = X_full_s[:n], y_full[:n]
        clf = SVC(kernel='rbf', C=10, gamma='scale')
        clf.fit(X_sub, y_sub)
        train_score = clf.score(X_sub, y_sub)
        # Test on all data
        test_score = clf.score(X_full_s, y_full)
        plot_decision_boundary(ax, clf, X_sub, y_sub,
                               f'n={n}  train={train_score:.0%}  '
                               f'test={test_score:.0%}',
                               cmap_bg, cmap_pts)

    plt.tight_layout()
    plt.savefig('ml_data_quantity.png', dpi=150, bbox_inches='tight')
    print("Saved: ml_data_quantity.png")
    plt.show()

    # -- Part 3: Overfitting demo --
    print("\n=== Overfitting Demo ===")
    fig3, axes3 = plt.subplots(1, 3, figsize=(18, 5.5))
    fig3.suptitle('Overfitting: Same Data, Different Model Complexity',
                  fontsize=13, fontweight='bold')

    X_small, y_small = make_keystroke_data(n_samples=30, n_classes=3,
                                           separation=1.5)
    X_small_s = StandardScaler().fit_transform(X_small)
    X_test, y_test = make_keystroke_data(n_samples=500, n_classes=3,
                                         separation=1.5, random_state=99)
    X_test_s = StandardScaler().fit_transform(X_test)

    overfit_models = [
        ('k-NN k=1 (overfit)', KNeighborsClassifier(n_neighbors=1)),
        ('SVM C=1 (balanced)', SVC(kernel='rbf', C=1, gamma='scale')),
        ('SVM C=1000 (overfit)', SVC(kernel='rbf', C=1000, gamma=1)),
    ]

    for ax, (name, clf) in zip(axes3, overfit_models):
        clf.fit(X_small_s, y_small)
        train_acc = clf.score(X_small_s, y_small)
        test_acc = clf.score(X_test_s, y_test)
        plot_decision_boundary(ax, clf, X_small_s, y_small,
                               f'{name}\ntrain={train_acc:.0%} '
                               f'test={test_acc:.0%}',
                               cmap_bg, cmap_pts)

    plt.tight_layout()
    plt.savefig('ml_overfitting.png', dpi=150, bbox_inches='tight')
    print("Saved: ml_overfitting.png")
    plt.show()


def interactive_demo():
    """Adjust number of samples and classifier parameters."""
    if not HAS_SKLEARN:
        print("scikit-learn required")
        return

    from matplotlib.widgets import Slider, RadioButtons

    cmap_bg = ListedColormap(['#ffaaaa', '#aaffaa', '#aaaaff',
                               '#ffffaa', '#ffaaff'])
    cmap_pts = ListedColormap(['#ff0000', '#00aa00', '#0000ff',
                                '#aaaa00', '#aa00aa'])

    fig, ax = plt.subplots(1, 1, figsize=(9, 7))
    fig.subplots_adjust(bottom=0.25, right=0.72)

    model_name = ['SVM']
    X_all, y_all = make_keystroke_data(n_samples=500, n_classes=5,
                                       separation=1.0)
    X_all = StandardScaler().fit_transform(X_all)

    def draw(n_samples, C=10):
        ax.clear()
        X = X_all[:n_samples]
        y = y_all[:n_samples]

        if model_name[0] == 'SVM':
            clf = SVC(kernel='rbf', C=C, gamma='scale')
        elif model_name[0] == 'RF':
            clf = RandomForestClassifier(n_estimators=50, random_state=42)
        else:
            clf = KNeighborsClassifier(n_neighbors=max(1, int(C)))

        clf.fit(X, y)
        train_acc = clf.score(X, y)
        test_acc = clf.score(X_all, y_all)

        plot_decision_boundary(ax, clf, X, y,
                               f'{model_name[0]} | n={n_samples} | '
                               f'train={train_acc:.0%} test={test_acc:.0%}',
                               cmap_bg, cmap_pts)
        fig.canvas.draw_idle()

    ax_n = fig.add_axes([0.15, 0.12, 0.45, 0.03])
    ax_c = fig.add_axes([0.15, 0.06, 0.45, 0.03])
    s_n = Slider(ax_n, 'Samples', 10, 500, valinit=100, valstep=10)
    s_c = Slider(ax_c, 'C / k', 0.1, 100, valinit=10)

    ax_radio = fig.add_axes([0.74, 0.4, 0.24, 0.2])
    radio = RadioButtons(ax_radio, ('SVM', 'RF', 'k-NN'), active=0)

    def update(_=None):
        draw(int(s_n.val), s_c.val)

    def model_changed(label):
        model_name[0] = label
        update()

    s_n.on_changed(update)
    s_c.on_changed(update)
    radio.on_clicked(model_changed)
    draw(100, 10)
    plt.show()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--interactive', '-i', action='store_true')
    args = parser.parse_args()
    if args.interactive:
        interactive_demo()
    else:
        static_demo()
