# NAM Parametric Plugin 0.5.2

Version 0.5.2 combines faster parametric-control interpolation with a discreet, opt-in update notification in the plugin UI. The **100/100 ms Linear** control behavior is unchanged.

## Faster real-time weight smoothing

- During movement of an A2/HyperWaveNet control, convolution kernels now calculate the delta-weight contribution using preallocated **Eigen matrix multiplications** (GEMM) instead of per-coefficient scalar products. The intermediate coefficients remain mathematically equivalent to the former per-sample Linear interpolation.
- Reuses one neural audio network, with at most one hypernetwork update every **100 ms** and a **100 ms Linear** coefficient ramp at the audio sample rate.
- An unmodified convolution skips the delta work entirely. The fast stationary convolution path remains intact.
- All scratch arrays are allocated before audio processing. This optimization does **not** use the PR's optional multithreaded/off-audio-thread hypernetwork API, additional audio DSP instances or crossfades.
- NeuralAudio's reference A2 implementation was tested in an isolated A/B/C run using the same inputs and sample-accurate automation: no meaningful audio regressions, with significantly lower smoothing CPU cost. The plugin uses its own pinned parametric core, so native plugin CI and real capture listening remain important.

## Quiet notification of newer versions

- The plugin checks the official `DLC86/NamParametricPlugin` GitHub releases **asynchronously on its UI background task**.
- When a published version newer than the installed version is found, a small **blinking red LED** appears beside the Settings button.
- Clicking the LED opens that version's official GitHub release page.
- **No popup or startup message**, no automatic update or download; the LED remains hidden if there is nothing newer or the network is unavailable. No network operation occurs in the audio callback.
- Published version metadata is checked using numeric semantic-version comparison. Drafts are ignored.

## Still supported

- Legacy `enum_names` switches and the `options` / `name` / `value` / `index_sw` format without modifying the neural switch encoding.
- `ParametricSlimmableContainer`, `HyperA2` and older parametric NAM files, Full/Lite mode and static NAM export.
- Input calibration, Auto output mode, corrected IR normalization and the independent Linear Volume attenuator.
- Session recall and the original JUCE VST3 / AU / Standalone plugin formats.

## Native downloads

The release workflow builds and verifies seven native packages from the same commit, then publishes the GitHub release:

- **Windows x64:** VST3 and Standalone.
- **macOS universal (Apple Silicon and Intel):** VST3, AU and Standalone.
- **Linux x86_64:** VST3 and Standalone.

Windows/macOS binaries are not code-signed or notarized. See the [README](https://github.com/DLC86/NamParametricPlugin#download) for installation instructions.
