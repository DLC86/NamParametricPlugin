# Static NAM export

Load a HyperWaveNet or HyperA2 model, set its knobs and switches, then click
**Export NAM...** in the model bar. Choose where to save the file. No recording,
training, Python installation or internet connection is needed.

The export captures the settings when the button is clicked, before the save
dialog opens. Weight generation and writing run in a background worker using an
immutable copy of the loaded model. Playback is not stopped, and later control
changes do not affect that export. Replacing/deleting the source on disk does
not change the model being exported.

- **HyperWaveNet:** writes a stock `WaveNet` with the conditioned weights.
- **HyperA2:** writes one stock `SlimmableContainer` with two complete WaveNets:
  Lite (3 channels, `max_value=0.5`), then Full (8 channels, `max_value=1.0`).
  Both are baked at the same settings, regardless of the currently selected size.
  A player must support `SlimmableContainer` to load this dual-size file. This
  plugin retains its Full/Lite selector when reloading an exported A2 file.

The suggested filename preserves the source basename and adds named settings:

`Pedal.nam` → `Pedal (Gain=6.5, Boost=On, Mode=C).nam`

Switches use their displayed label; continuous values use the shortest decimal
that round-trips to the float used by the model. Characters disallowed in Windows
filenames are replaced with underscores. You can edit the destination name in the
save dialog. Export refuses to overwrite the original parametric file and uses
a temporary file beside the destination so a failed generation/write does not
truncate an existing model.

The export fixes the **model's controls**, including its existing output scale.
The plugin's Input/Output gain, calibration settings, and external cabinet IR are
separate playback settings and are not baked into the network. Original metadata,
including calibration and loudness, is preserved; loudness is not remeasured for
the new settings. The generated weights reproduce the original model with the
same controls, subject to floating-point differences.

## Custom metadata

Both the top-level model and each A2 child preserve the original metadata and add
`metadata.nam_parametric_plugin.static_export`:

```json
{
  "schema_version": 1,
  "source_file": "Pedal.nam",
  "source_architecture": "HyperA2",
  "variants": ["lite", "full"],
  "controls": [
    {"name": "Gain", "type": "continuous", "value": 6.5},
    {"name": "Boost", "type": "switch", "value": 1.0, "label": "On"}
  ]
}
```

`value` is the actual numeric model value (the zero-based enum index for a
switch); `label` is also included for switches. Controls retain their model order.
`variants` is present only for HyperA2. Metadata is informational; a standard NAM
player does not need to interpret it to render the static model.

## Verification

`static_export_test` creates deterministic full/low-rank HyperWaveNet and HyperA2
models with a knob and two-/three-position switches. It checks fixed-weight audio
against the live parametric model, both branches through the stock container
loader, round-trip loading, head scale, provenance and preserved metadata. It also
checks filename generation, immutable snapshots, failed writes, source protection,
and the export button layout at the minimum and default editor widths.

The Branch Build workflow runs this regression before packaging Windows and macOS
binaries and uploads the generated test models and UI previews separately.
