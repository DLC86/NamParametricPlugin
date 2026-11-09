# NAM Parametric Plugin 0.5.0

Version 0.5.0 updates parametric-model compatibility and corrects the large output-level drop that could occur when enabling an impulse response (IR).

## What's new

### New ParaNAM Trainer model format
- Loads the new `ParametricSlimmableContainer` format produced by current ParaNAM Trainer exports.
- Preserves compatibility with legacy `HyperA2` and `ParametricOutputGain` models.
- Supports optional `extensions.linear_volume` curves: the measured clean volume taper is applied **after** the neural model, with smoothing to avoid clicks. Volume-only adjustments do not require regenerating neural-network weights.
- Keeps Full/Lite model selection, input calibration, session recall, and static NAM export.

### IR loader: corrected output gain
- Compensates for the **-18 dB** attenuation previously introduced by AudioDSPTools' IR processing path.
- Uses the same IR normalization target as the **Compare** page in ParaNAM Trainer: average spectral power across **64 logarithmically spaced bands from 100 Hz to 5 kHz**.
- Calculates the correction outside the real-time audio thread and updates it when the sample rate changes.
- Leaves the IR stage optional and keeps the plugin's output-gain control independent.

**Note:** The target is approximately **0 dB average spectral gain** over the comparison band, not a guarantee of identical peak level or perceived loudness for every IR. An IR's frequency response can still change perceived volume.

### Regression coverage and packaging
- Adds IR gain-normalization regression tests.
- Verifies model loading, audio processing, static export, and calibration/control behavior in native builds.

## Downloads

Seven native packages are built and published only after their platform checks pass:

- **Windows x64:** VST3 and Standalone
- **macOS universal (Apple Silicon + Intel):** VST3, AU, and Standalone
- **Linux x86_64:** VST3 and Standalone

Windows and macOS binaries are not code-signed/notarized; see the [README](https://github.com/DLC86/NamParametricPlugin#download) for installation instructions.

This 0.5.0 release follows [v0.4.5](https://github.com/DLC86/NamParametricPlugin/releases/tag/v0.4.5). It is published as a **preview**, consistent with this repository's release workflow, and does not replace the currently marked Latest release.
