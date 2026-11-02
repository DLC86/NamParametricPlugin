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

The update rules also match NAM Oversampler: changing Input calibration or
Calibrate input updates the **input** gain only. It does not recompute the
output correction at the same time. Calibrated/Auto output takes its reference
when a model is activated or Output mode/Output level changes. This preserves
the reference plugin's behavior, including when recalibrating a loaded model.
Raw and Normalized output corrections never depend on input calibration.
Gear types are case-insensitive, and legacy `return_level`/`return_level_dbu`
metadata is accepted when `output_level_dbu` is absent, as in NAM Oversampler.

Changing drive can still change the audible level through the modeled gear's
compression/distortion; this is independent of the output gain correction.

## Smooth HyperWaveNet / HyperA2 controls

### 0.4.1 Eco de-click

Settings > **Control smoothing** now defaults to **Eco de-click** for new instances
and sessions without a saved mode. Only one audio network processes audio. The
standby instance remains allocated for mode comparison but is not run in Eco.
The implementation adds no lookahead or audio latency.

At a committed control change the model installs the destination weights once.
A short linear predictor estimates the boundary step. Residuals inside the
signal's normal prediction-error envelope are ignored, avoiding false corrections
on ordinary high-frequency audio. A detected excess is subtracted with a finite
5 ms smoothstep tail at the model sample rate, ahead of resampling. Rapid updates
carry the remaining correction into the next tail. The transient is bounded by
the recent raw-audio peak; uncorrected model audio is never limited or filtered.

This is an experimental, inexpensive de-click, not a continuous interpolation of
the complete nonlinear transfer function. Changes deeper in the convolution
history and large timbral moves may still be audible. **Crossfade** remains
selectable for comparison. It finishes a fade already in flight before entering
Eco; entering Crossfade first warms the sleeping standby inaudibly at the current
controls. Parameter changes during that short warmup are queued. Reset settles
immediately on the committed controls and clears correction tails.

Tests cover Eco's response to steps, rapid mixed knob/switch updates, both model
sizes, odd and large buffers, resampling, mode changes, exact settled response and
an unchanged audio signal while irrelevant controls move. Runtime probes compare
raw, Eco and Crossfade with identical models, plus raw/Eco at the same control
update rate. Session/UI tests cover the saved mode and the Eco initial setting.

Indicative local measurement, 2026-11-02, Windows Release build on an Intel
Core i7-12700H: the deterministic 3-channel fixture, 400 blocks of 257 samples,
48 kHz, no IR or device I/O. Raw network: 105.5 ms; Eco: 112.1 ms (1.06x);
Crossfade: 218.2 ms (2.07x). At the same moving-control update rate, raw took
111.0 ms and Eco 114.5 ms (1.03x). These short probes are not a promise for
every CPU/model or a listening-quality assessment; the test prints reproducible
comparisons on each build machine. On the DC-step fixtures Eco reduced the
largest adjacent-sample change to approximately 0.0063 of the hard step.

### Crossfade mode (0.4.0 behavior)

Both architectures use a 50 ms, per-sample smoothstep crossfade between two
audio networks at valid control settings. New weights are installed only in
the currently inaudible network. Both receive the same incoming audio, so
their convolution histories remain live. Repeated moves retain the latest
complete control vector and finish the current fade before starting the next.
This avoids restarting a fade partway through or quantizing a control ramp to
the host's buffer boundaries. Switch indices/one-hot encodings remain discrete;
the audio is blended, rather than inventing intermediate switch positions.

The fade runs at the model sample rate, ahead of host-rate resampling, with
buffers allocated during Reset. Reset and session restore settle directly on
the requested controls. Settled audio and static exports retain the original
model response. Static NAM and ConcatWaveNet playback are unchanged.

Keeping two complete histories live costs approximately twice the audio-network
CPU and memory for HyperWaveNet/HyperA2 (only the selected Full or Lite size is
duplicated). This is an explicit quality/cost tradeoff, not free parameter
smoothing. The regression tests report the measured cost and the 1–20 kHz
step spectrum relative to a hard parameter jump on deterministic fixtures;
they also exercise fast retargets, multiple switches, odd/large host buffers,
44.1/48/96 kHz playback and the final settled response.

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
