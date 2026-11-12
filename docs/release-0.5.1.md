# NAM Parametric Plugin 0.5.1

**Draft release for testing.** This release remains unpublished.

## Single-network linear weight smoothing

- HyperWaveNet/HyperA2 use **one neural audio model**, not two copies with crossfade.
- The hypernetwork recalculates target weights at most once every **100 ms of model audio** while controls move; intermediate knob changes are coalesced to the latest destination.
- Convolution coefficients interpolate **linearly at every audio sample over 100 ms**; the original trained weights and sound at stationary control positions remain unchanged.
- Removes the old Eco output-step correction, two-network Crossfade DSP, and experimental **Control smoothing**, timing and curve settings. The same fixed 100/100 ms Linear behavior is used regardless of older session settings.
- The independent measured **Linear Volume** output attenuator remains available, with its own fast gain smoothing and without regenerating neural weights.

## Switch metadata and backwards compatibility

- New parametric models may specify each switch's `options` as objects with `name`, `value` (original neural switch state) and `index_sw` (zero-based visual order). For example `{"name":"Crunch","value":1,"index_sw":0}`.
- Renaming/reordering a switch option in the display **does not change** its neural one-hot encoding, static-export position, or session value.
- Still loads older `enum_names` arrays and `HyperA2` / `ParametricSlimmableContainer` exports. Optional `config.params[].index` continues to determine knob/switch/Linear Volume presentation order.
- Existing sessions continue to restore model/knob/IR/calibration settings; obsolete experimental transition settings are ignored.

## Other included improvements

- Preserves the 0.5.0 IR unity-gain correction, output modes including Auto, input calibration, Full/Lite support, static NAM export and original metadata.
- No model retraining or conversion is required for old captures.

## Native packages

- **Windows x64:** VST3 and Standalone
- **macOS universal (Apple Silicon and Intel):** VST3, AU, Standalone
- **Linux x86_64:** VST3 and Standalone

These are seven separately packaged native builds from the same verified release commit. Windows and macOS binaries are unsigned and are not notarized. This is an unpublished draft; **do not publish without explicit approval**.
