# NamParametricPlugin

**Version 0.5.2 (development prototype, not released)** replaces Eco's output-side step predictor
with **single-network weight interpolation** for HyperWaveNet / HyperA2.
The hypernetwork generates target weights at most once every 20 ms, and
the *same* WaveNet interpolates between active and target coefficients over
40 ms, with one update per 0.5 ms of model audio. Rapid retargets start from
the currently applied weights, without running a second audio model. The
original Crossfade mode remains available for direct comparisons, with its
two-network behavior unchanged. This approach trades some additional
weight-copy CPU during knob movement for avoiding hard weight jumps.
No model format change or retraining is required. Sound quality and resource
costs depend on the model and host hardware; this draft is intended for
testing, especially with zero digital input.

**Version 0.5.1 (draft)** adds a model-sample-time rate limiter to the
default **Eco de-click** mode: neural weights are reconditioned at most once
every **20 ms**. Rapid knob and switch automation is coalesced to the latest
complete value vector, with the first change applied immediately and the
original 5 ms de-click tail preserved. Linear Volume still uses its separate
sample-smoothed DSP stage (no hypernetwork reconditioning), while the
two-network Crossfade mode is unchanged. This stabilizes the frequency of
CPU spikes during rapid automation; it does not move the individual weight
calculation off the audio thread. The rate limit introduces up to about
20 ms of additional control response during sustained movement, quantized
to model processing block boundaries, but no audio latency.

**Version 0.5.0** adds support for new
`ParametricSlimmableContainer` models (while still loading older `HyperA2`
and `ParametricOutputGain` files), plus optional
`extensions.linear_volume` output attenuation. The measured Linear Volume
curve is evaluated after the neural model, with click-free smoothing;
Full/Lite, calibration, and static export remain supported.

The plugin also recognizes optional `config.params[].index` metadata
exported from the ParaNAM Trainer Plan page. It arranges neural knobs,
switches and Linear Volume by one shared zero-based display order while
keeping the original neural parameter indices for audio processing, session
recall and static NAM export. Models without these new indices retain
their previous layout. The new 0.5.1 draft also includes this ordering support.

The IR loader now compensates for AudioDSPTools' built-in −18 dB gain and
normalizes IR response using the same **64 log-spaced 100–5000 Hz band-power**
target as ParaNAM Trainer's **Compare** page. This targets a **0 dB mean
spectral gain**, not identical perceived loudness at every frequency. The
correction is computed off the real-time audio thread and recalculated
whenever the sample rate changes. Unlike previous versions, enabling an IR
should no longer cause an unexplained large drop in output level.
IR convolution is still a separate optional stage and the plugin's output
gain knob remains independent.

The prior interface update introduced a #bf0000 red accent, neutral-gray
surfaces and the TTS/NAM On Steroids logo.

JUCE-based `VST3 + AU + Standalone` plugin for loading and running Neural Amp Modeler (`.nam` and `.pnam`) models,
including parametric NAM models via `nam_core_parametric`.

## Credits

This fork is based on **[Phillip Self](https://github.com/phillipmself)**'s
[NAM Parametric Plugin](https://github.com/phillipmself/NamParametricPlugin) and
[NeuralAmpModelerCoreParametric](https://github.com/phillipmself/NeuralAmpModelerCoreParametric),
which introduced its parametric plugin and inference foundations. The underlying
NAM ecosystem and AudioDSPTools were created by **Steven Atkinson** and contributors.
The current HyperA2 fast path also incorporates work derived from **Mike Oliphant**'s
NeuralAudio. DLC86 maintains this fork and its extensions.

See [CREDITS.md](CREDITS.md) for the full attribution overview and
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md) for license notices. Current release
sources are maintained on [main](https://github.com/DLC86/NamParametricPlugin/tree/main).

## Related Projects

This plugin builds on two companion repositories:

- [neural-amp-modeler-parametric](https://github.com/phillipmself/neural-amp-modeler-parametric) —
  trains and generates the parametric `.nam` models this plugin loads.
- [NeuralAmpModelerCoreParametric](https://github.com/phillipmself/NeuralAmpModelerCoreParametric) —
  the core inference code (vendored here as `nam_core_parametric`) that allows the plugin to run
  those parametric models.

## Download

Prebuilt builds are published on the
[Releases page](https://github.com/DLC86/NamParametricPlugin/releases).

### macOS (universal: `arm64` + `x86_64`)

1. Download `NAM-Parametric-Plugin-<version>-macOS-VST3.zip`, `...-macOS-AU.zip`, and/or
   `...-macOS-Standalone.zip` (standalone app).
2. Unzip, then move the plugin into the matching folder (or the `.app` wherever you like):
   - VST3: `~/Library/Audio/Plug-Ins/VST3/`
   - AU: `~/Library/Audio/Plug-Ins/Components/`
3. **These builds are unsigned/unnotarized**, so macOS Gatekeeper will refuse to open them the
   first time. To allow it:
   - Right-click (or Control-click) the `.app`/`.vst3`/`.component` and choose **Open**, then
     confirm in the dialog that appears, **or**
   - Run in Terminal:
     ```bash
     xattr -cr "/path/to/NAM Parametric Plugin.app"
     xattr -cr ~/Library/Audio/Plug-Ins/VST3/"NAM Parametric Plugin.vst3"
     xattr -cr ~/Library/Audio/Plug-Ins/Components/"NAM Parametric Plugin.component"
     ```
   You only need to do this once per download.

### Windows (x64)

1. Download `NAM-Parametric-Plugin-<version>-Windows-VST3.zip` and/or
   `...-Windows-Standalone.zip`.
2. Unzip, then move the `.vst3` folder into `C:\Program Files\Common Files\VST3\` (or run the
   standalone `.exe` wherever you like).
3. **This build is unsigned**, so Windows SmartScreen may warn that it's from an unrecognized
   publisher. Click **More info**, then **Run anyway**. You only need to do this once per download.

### Linux (x86_64)

1. Download `NAM-Parametric-Plugin-<version>-Linux-x86_64-VST3.tar.gz` and/or
   `...-Linux-x86_64-Standalone.tar.gz` from this fork's Releases page.
2. Extract the VST3 archive and place the complete `NAM Parametric Plugin.vst3`
   directory in `~/.vst3/`, then rescan plugins in your Linux DAW.
3. Extract the standalone archive and run `NAM Parametric Plugin`. The tar archive
   preserves its executable permission.

Builds are tested on Ubuntu 22.04 with X11 (or XWayland) and ALSA/JACK support.
Other distributions need compatible system audio, X11 and font libraries.
The Linux presets use Clang/LLD because the vendored resampling library's type
aliases are rejected by GCC. GitHub Actions runs the audio/control/calibration
regression tests under a virtual display and checks for missing runtime libraries.

## Models

Parametric `.nam` models are published as their own releases, separate from the plugin builds, on the
[Releases page](https://github.com/phillipmself/NamParametricPlugin/releases). Model releases use a
`models-*` tag so they version independently of the plugin.

**Latest:**
[5153 100W (Blue) — Parametric Models v1](https://github.com/phillipmself/NamParametricPlugin/releases/tag/models-5153-v1)

- `5153_100w_Blue_ConcatWaveNet_BoostInput+4dB.nam` — ConcatWaveNet capture.
- `5153_100w_Blue_HyperWaveNet_BoostInput+4dB.nam` — HyperWaveNet capture.
- `models-5153-v1.zip` — both of the above.

To use a model, download a `.nam` file and load it in the plugin (see [Usage Notes](#usage-notes)).

> **Input calibration:** These models run a little quiet on the input. Add roughly **4–6 dB of input
> gain** for them to respond like a real 5153. Future model releases will be more accurately
> input-calibrated, so this extra input gain won't be needed.

## Scope (v1)

- Mono input/output processing path.
- Host-automatable parameters:
  - `Input Gain` (`-12 dB` to `+12 dB`)
  - `Output Gain` (`-12 dB` to `+12 dB`)
- Model load via file chooser (`.nam`).
- Parametric models may use **`.pnam`**, as exported by ParaNAM Trainer. Legacy
  parametric `.nam` files still load; the content and metadata format are the same.
  **Export NAM...** continues to produce a static **`.nam`**.
- Dynamic model parameters are generated in the plugin UI and applied at runtime.
- **Control smoothing** in Settings defaults to experimental **Eco de-click** in 0.4.1:
  one running network and a 5 ms correction of output steps when controls change.
  **Crossfade** retains the 0.4.0 behavior (two running networks, approximately
  twice the model DSP CPU) for listening comparisons. The session recalls the choice.
- **Export NAM...** freezes HyperWaveNet controls into a static WaveNet, or HyperA2
  controls into one static file containing **both Lite and Full**. The suggested
  filename includes the settings; custom metadata records every control name and
  value. No retraining is required. See [Static NAM export](docs/Static-NAM-export.md).
- `HyperA2` parametric files from the [DLC86 trainer](https://github.com/DLC86/neural-amp-modeler-parametric/tree/codex/parametric-a2)
  load with **Full (8 channels) selected by default**, independently of the trainer's
  playback preference. The model bar exposes **Full / Lite (3 channels)** for these files.
  Changing size loads the replacement in the background and retains current knob values.
  Sessions remember an explicitly selected Lite mode; older sessions default to Full.
- **Settings** contains **Input calibration**, **Calibrate input** and **Output mode**,
  using NAM Oversampler's level equations and update rules: changing Input calibration
  updates drive without simultaneously recomputing the output trim. Details are in
  [HyperA2 and level calibration](docs/HyperA2-and-calibration.md).
- Dynamic model parameters are **not** exposed as host automation parameters in v1.
- Plugin state persists:
  - APVTS state
  - `modelPath` (absolute path)
  - dynamic model parameter values by ordered index and name
  - HyperA2 Full/Lite selection and level-calibration settings
- Sample-rate mismatch between DAW and model is handled via AudioDSPTools resampling.

## Repository Layout

- `Source/`: plugin processor/editor and NAM integration layer.
- `external/JUCE`: JUCE submodule.
- `external/nam_core_parametric`: NAM core with parametric support.
- `external/AudioDSPTools`: resampling container dependency.

## Prerequisites

macOS:
- Xcode command line tools
- CMake `>= 3.22`
- Ninja
- `clang-format` (for local formatting consistency)

Windows:
- Visual Studio (Desktop development with C++ workload) or the standalone MSVC Build Tools
- Ninja
- CMake `>= 3.22`
- Run from a "Developer PowerShell/Command Prompt for VS" (or run `vcvarsall.bat`/`Enter-VsDevShell` first) so `cl.exe` is on `PATH`

## Setup

Initialize submodules (required):

```bash
git submodule update --init --recursive
```

## Build

macOS debug build:

```bash
cmake --preset mac-debug
cmake --build --preset mac-debug -j 8
```

macOS release build:

```bash
cmake --preset mac-release
cmake --build --preset mac-release -j 8
```

Windows release build:

```bash
cmake --preset windows-release
cmake --build --preset windows-release
```

## Model Smoke Test

The smoke-test executable loads one `ConcatWaveNet` and one `HyperWaveNet` model, renders at a
mismatched host sample rate with a non-power-of-two block size, applies alternate full parameter
vectors, and checks for finite, non-silent, parameter-dependent output:

```bash
./build/mac-debug/nam_model_smoke_test /path/to/concat.nam /path/to/hyper.nam
```

To register the same check with CTest, configure with model paths and then run CTest:

```bash
cmake --preset mac-debug \
  -DNAM_CONCAT_TEST_MODEL=/path/to/concat.nam \
  -DNAM_HYPER_TEST_MODEL=/path/to/hyper.nam
ctest --test-dir build/mac-debug --output-on-failure
```

## Run (Standalone)

After a debug build, launch:

```bash
./build/mac-debug/NamParametricPlugin_artefacts/Debug/Standalone/NAM\ Parametric\ Plugin.app/Contents/MacOS/NAM\ Parametric\ Plugin
```

VST3 / AU output paths (debug):

```text
build/mac-debug/NamParametricPlugin_artefacts/Debug/VST3/NAM Parametric Plugin.vst3
build/mac-debug/NamParametricPlugin_artefacts/Debug/AU/NAM Parametric Plugin.component
```

## Usage Notes

1. Open plugin.
2. Click `Load .nam Model`.
3. Choose a `.nam` file.
4. If the model is parametric, dynamic controls appear under `Model Parameters`.
5. Adjust `Input Gain` / `Output Gain` as needed (bounded to `-12/+12 dB`).

Multiple switches can coexist with continuous knobs. Each switch shows one button per
named position, with one selected at a time; selecting a position leaves the other
switches unchanged. All combinations of positions are selectable. The panel scrolls
vertically for multiple rows and horizontally when a single switch is too wide, so
every position stays reachable. Session saves and HyperA2 Full/Lite changes preserve
each control's selection.

### Multi-switch integration test

Generate actual trainer exports and reference audio for all 30 combinations of
three switches (3, 2 and 5 positions), mixed with two knobs:

```bash
python Tests/generate_switch_fixture.py --trainer-dir /path/to/neural-amp-modeler-parametric --output build/test-fixtures
```

Configure with `-DNAM_HYPER_A2_TEST_DIR=/absolute/path/to/build/test-fixtures` and
`-DNAM_PARAMETRIC_BUILD_TESTS=ON`, then build `hyper_a2_processor_test`. Run
`ctest --test-dir <build-directory> -C Release -R switch_combinations --output-on-failure`.
The test clicks the real controls and checks audio against the trainer for
HyperA2 Full/Lite, HyperWaveNet and ConcatWaveNet (120 combinations in total).
It also checks independent defaults, session restoration, selection preservation
across A2 size changes, and access to all positions of a wide 20-position switch.

## Manual Validation Checklist

Run these checks before merging major changes:

1. Build succeeds with `cmake --preset mac-debug` and `cmake --build --preset mac-debug`.
2. Plugin loads with no model and processes audio safely (pass-through + gains).
3. Gains are bounded to `-12/+12 dB` and audible as expected.
4. Non-parametric model loads; dynamic section shows no controls.
5. Parametric model loads; continuous sliders and switch menus appear and audibly affect output.
6. Model sample-rate mismatch still processes audio without failure (resampling active).
7. Save/reload session restores gains, `modelPath`, and dynamic values by name.
8. Missing/broken stored `modelPath` does not crash and plugin remains usable.

## Known Limitations (v1)

- Dynamic model parameters are UI-only (no host automation IDs).
- Mono-only internal processing.
- Model load is chooser-only (no drag/drop).
- Windows build is CI-built but less battle-tested than the macOS path (macOS is the primary dev environment).

Windows standalone builds include ASIO alongside WASAPI and DirectSound. Select
ASIO in the audio settings, then choose the interface driver. The interface must
have its own compatible 64-bit ASIO driver installed.
