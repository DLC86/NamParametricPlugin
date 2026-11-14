# PNAM Player 0.5.4

This release changes the product name from NAM Parametric Plugin to **PNAM Player** and identifies the vendor as **The Tone Scientist** (manufacturer code `TTSc`), matching NAM Oversampler's vendor grouping. The unique plug-in code is now `PnPl`, distinct from Phillip Self's upstream `NpPg`.

- The main GUI title **PNAM Player** is entirely red, the version is shown beneath it, and the red update indicator appears beside the version.
- Input Calibration uses an editable numeric field without a slider, with 0.001 dBu resolution and exactly three displayed decimal places. The editable value stays centered when focused; its background and the Output Mode selector use the dark model-parameter-area color. The parameter ID is unchanged.
- The earlier model metadata fix is included: input/output calibration values and gear type are read from outer metadata, while loudness is selected from the active Lite/Full model with fallback to outer loudness.
- The neural audio engine, effects and model compatibility remain unchanged.

**DAW session migration:** Changing product name and manufacturer/plug-in IDs may make PNAM Player appear as a different plug-in from the previous release. Back up projects that use the older plugin before migrating.

**Packages:** Windows x64 VST3 and Standalone; macOS universal VST3, AU and Standalone; Linux x86_64 VST3 and Standalone. The seven packages are built without repeating previously completed model, calibration, audio, IR or export functional tests. The release **stays draft and is not published**.
