# HyperA2 and level calibration

## Full and Lite

Load `model_parametric.nam` exported by the HyperA2 trainer. The plugin
validates both submodel layouts and loads the selected branch through the
existing HyperWaveNet runtime. No separate core fork or new submodule commit
is required.

**Full is the default** for a new instance, for an old session without a size
preference, and even if the model's training export says `size: lite`.
Full runs the original 8-channel A2 network; Lite runs the original 3-channel
network. Both retain all 23 layers, kernels, dilations and the 16-tap head.

The model bar shows a Full/Lite selector for HyperA2 models. Size changes
reuse the background loading path, leaving construction, reset and prewarm
outside audio processing. Current controls, including changes made while
the replacement loads, are carried to the new network before activation.
Only the selected audio network runs. The session saves the selection.
Changing size is a model replacement, not a continuous crossfade effect.

Regular WaveNet, HyperWaveNet and ConcatWaveNet loading remains available.

## Settings and calibration

The **Settings** button opens:

* **Calibrate input**: enabled by default, as in standard NAM.
* **Input calibration**: your playback interface's analog input level in dBu
  corresponding to 0 dBFS. The standard default is +12 dBu; the range is
  -60 to +60 dBu. Set this to your actual interface/gain setting.
* **Output mode**: Raw, Normalized, Calibrated or Auto (default).

These settings are stored with the plugin session. Input/Output knobs remain
independent manual trims.

The standard metadata keys are `metadata.input_level_dbu` and
`metadata.output_level_dbu`, not `input_dbu` and `output_dbu`. They work for
both HyperA2 sizes and for ordinary NAM models. Missing metadata supplies no
calibration offset; an unknown calibration is never invented.

The gain equations match standard NAM:

```text
input dB = Input knob + interface input calibration - model input_level_dbu
```

The automatic input term is included only if Calibrate input is enabled and
the model has input metadata. Output modes add to the Output knob:

| Mode | Automatic output term |
| --- | --- |
| Raw | 0 dB |
| Normalized | -18 dBFS minus model loudness, when known |
| Calibrated | model output_level_dbu minus interface input calibration, when known |
| Auto | Normalized for amp/pedal_amp/amp_cab/amp_pedal_cab; otherwise Calibrated |

For example, an interface input calibration of +12 dBu and a model input
reference of +18 dBu produce a -6 dB input correction. A model output
reference of +6 dBu produces a -6 dB correction in Calibrated output mode.

In the capture app, the **Project** tab can record optional Input and Output
levels at 0 dBFS. Input describes the capture's D/A/reamp path at the device
input; Output describes the recording A/D path. These are properties of the
capture chain, whereas the plugin's Input calibration describes the current
playback interface. The trainer carries the values into parametric exports,
baked NAM exports and checkpoint NAM files, including when training targets
were normalized.

## Verification

Generate fixtures with the HyperA2 trainer checkout:

```sh
python Tests/generate_hyper_a2_fixture.py --trainer-dir /path/to/trainer --output build/test-fixtures
cmake --preset windows-release -DNAM_HYPER_A2_TEST_DIR=/absolute/path/to/build/test-fixtures
cmake --build --preset windows-release
ctest --test-dir build/windows-release --output-on-failure
```

For a multi-configuration generator, build and test with `--config Release`
and `ctest -C Release`. The fixture generator needs the trainer's PyTorch
environment; it is not needed to build or run the plugin.

Tests compare C++ audio with PyTorch exports in both hypernetwork modes
(`low_rank` and `full`), both audio sizes and two control settings. They also
cover malformed files, resampling with odd buffer sizes, Full defaults,
rapid size changes, control preservation, session restore, and the standard
input/output calibration equations and actual calibrated audio.
