# Signal Processing & ML Interactive Demos

Visual simulations for the [Signal Processing Reference](../../../docs/Linux%20in%20Embedded%20Systems/Reference/signal-processing.md) and [ML Reference](../../../docs/Linux%20in%20Embedded%20Systems/Reference/ml-signal-processing.md).

## Quick Start

```bash
pip3 install numpy matplotlib scipy scikit-learn
cd scripts/signal-processing-demo

# Static plots (saved as PNG)
python sampling_aliasing.py
python fft_windowing.py
python filter_response.py
python ml_decision_boundary.py
python mel_spectrogram_explorer.py

# Interactive mode (matplotlib sliders)
python sampling_aliasing.py -i
python fft_windowing.py -i
python filter_response.py -i
python ml_decision_boundary.py -i
python mel_spectrogram_explorer.py -i
```

## Demos

| Script | What it teaches | Key concept |
|--------|----------------|-------------|
| `sampling_aliasing.py` | Nyquist theorem, aliasing | Must sample at ≥ 2× signal frequency |
| `fft_windowing.py` | Spectral leakage, window functions | Hann/Hamming/Blackman tradeoffs |
| `filter_response.py` | HP/LP/BP filters, EQ, biquad | Cutoff, order, 1-pole vs Butterworth |
| `ml_decision_boundary.py` | SVM/RF/k-NN boundaries, overfitting | Data quantity vs model complexity |
| `mel_spectrogram_explorer.py` | Step-by-step spectrogram construction | Linear → mel scale, filterbank, CNN input |

Each script runs in two modes:
- **Static** (default): generates publication-quality PNG plots
- **Interactive** (`-i`): matplotlib GUI with sliders to explore parameters in real time
