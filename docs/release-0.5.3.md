# NAM Parametric Plugin 0.5.3

## macOS Standalone: restore CoreAudio input access

- **Fix missing macOS microphone/audio-input privacy usage declaration.** The standalone application's `Info.plist` now contains `NSMicrophoneUsageDescription` with a clear explanation that access to the audio interface is needed to process a guitar or other instrument. This enables macOS to request and manage the appropriate permission when the standalone uses CoreAudio inputs.
- **Do not change the intentional default mute.** As in previous versions, the standalone initially mutes incoming audio to prevent feedback, and shows JUCE's existing notification and **Settings...** control. The user must select the right audio interface and explicitly clear **Mute audio input** in the standalone's **Audio/MIDI Settings**. Previously chosen mute/unmute settings remain managed by JUCE.
- Add a macOS build-time regression gate that checks the bundled app's `Info.plist` for the microphone permission text, the expected version and an unchanged JUCE default-mute wrapper.
- **AU/VST3 processing and the neural DSP are unchanged.** No control smoothing, weight interpolation, driver selection, audio processing, update LED, or output gain behavior has been modified.

### Troubleshooting a silent macOS Standalone

1. Open the standalone app, click the **Audio input is muted to avoid feedback loop** notification's **Settings...** button, and disable **Mute audio input** only when you are ready to monitor the instrument.
2. In **Audio/MIDI Settings**, select the **PreSonus Studio 26c** for both input and output, select the active guitar/instrument input channel, and confirm the buffer/sample-rate selection. Check the physical **Main** output, instrument gain, headphones and direct monitoring as appropriate.
3. In **System Settings → Privacy & Security → Microphone**, allow **NAM Parametric Plugin** to receive audio input. If previously denied, quit and relaunch the standalone after allowing it.
4. Update **PreSonus Universal Control** to the version supporting macOS 27 Golden Gate; the vendor officially lists the Studio 26c as supported.
5. To isolate an audio device issue, retry with another input/output device and check that the standalone receives an input signal with the model and IR bypassed.

**Important:** This fixes a concrete missing macOS privacy declaration, but we cannot guarantee that it resolves every silent-audio report without a test on the affected macOS 27.2.1 / Studio 26c system.

## Unchanged from 0.5.2

- Single-network HyperA2 with **100 ms weight updates and 100 ms sample-accurate Linear smoothing** optimized by Eigen GEMM, without the optional multithreaded hypernetwork.
- Quiet blinking update LED (no update popup at startup).
- Parametric switch `index_sw` / `options` schema, Full/Lite, IR normalization, Linear Volume and static NAM export.

## Packages

Seven verified native packages, built from the same release commit:

- Windows x64: VST3 and Standalone
- macOS universal (Apple Silicon + Intel): VST3, AU and Standalone
- Linux x86_64: VST3 and Standalone

Windows/macOS packages are not signed or notarized. See the [README](https://github.com/DLC86/NamParametricPlugin#download) for installation information.
