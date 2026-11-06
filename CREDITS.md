# Credits and acknowledgements

Credits for the DLC86 NAM Parametric Plugin fork and its `Knobs+Switch` release
branch. This project builds on the following authors' work; attribution does not
imply endorsement, and each component retains its original license.

## Original work and fork contributions

| Author / project | Contribution |
| --- | --- |
| **[Phillip Self](https://github.com/phillipmself)** — [NamParametricPlugin](https://github.com/phillipmself/NamParametricPlugin) | Original JUCE parametric plugin, model-loading/inference integration and dynamic-control foundations. The original MIT license, **Copyright (c) 2026 Phillip Self**, is retained in [LICENSE](LICENSE) and reproduced in the third-party notices shipped with plugin/app builds. |
| **Phillip Self** — [NeuralAmpModelerCoreParametric](https://github.com/phillipmself/NeuralAmpModelerCoreParametric) and [neural-amp-modeler-parametric](https://github.com/phillipmself/neural-amp-modeler-parametric) | Parametric NAM inference, hypernetwork/model support, and companion training/capture implementation. |
| **[Steven Atkinson](https://github.com/sdatkinson)** and NAM contributors — [NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore), [Neural Amp Modeler](https://github.com/sdatkinson/neural-amp-modeler), [AudioDSPTools](https://github.com/sdatkinson/AudioDSPTools) | Original NAM/A2 model and DSP infrastructure, static-model ecosystem and resampling library. |
| **[Mike Oliphant](https://github.com/mikeoliphant)** — [NeuralAudio](https://github.com/mikeoliphant/NeuralAudio) | Multi-frame convolution optimization adapted into this fork's HyperA2 fast path. The original MIT copyright and license are reproduced in the distributed notices. |
| **[DLC86](https://github.com/DLC86)** — [NAM Parametric Plugin fork](https://github.com/DLC86/NamParametricPlugin), [ParaNAM Trainer](https://github.com/DLC86/ParaNAM-Trainer), [NAM On Steroids / NAM Oversampler](https://github.com/DLC86/NAM-Oversampler) | HyperA2 Full/Lite integration and fast path, switches, static export, calibration, de-click, UI and packaging extensions. Calibration equations and UI/branding references derive from NAM On Steroids. |

The exact inference-core, JUCE and AudioDSPTools revisions are pinned by Git
submodules. Upstream history and source-file notices retain additional authors'
contributions.

## Framework, formats and supporting libraries

| Component / authors | Use and licensing |
| --- | --- |
| **JUCE contributors / Raw Material Software Limited** | Plugin, desktop UI and audio framework. JUCE's AGPL-3.0/commercial licensing terms and dependency notices are in `external/JUCE/LICENSE.md`. |
| **Steinberg Media Technologies GmbH** | VST3 SDK (MIT) and ASIO SDK (GPL-3.0 option in this project's open-source Windows builds). |
| **Apple / AudioUnitSDK contributors** | AudioUnit wrapper SDK used by macOS AU builds; Apache-2.0, with upstream notices retained. |
| **Eigen authors and contributors** | Linear algebra; built with the MPL-2.0 subset selected by `EIGEN_MPL2_ONLY`. |
| **iPlug 2 developers**, **Oli Larkin**, **John Schwartz / Cockos**, **Cockos WDL contributors**, **Paul Walker (baconpaul) / Surge Synth Team** | ResamplingContainer, WDL buffer utilities and the adapted Lanczos resampler used through AudioDSPTools. The original notices and the Lanczos file's specific MIT/BSD-context permission are preserved in the distribution notices. |
| **Niels Lohmann and contributors** | nlohmann/json model and metadata parsing; MIT. |
| **libFLAC, libogg and libvorbis authors / Xiph.Org** | JUCE's bundled audio-codec dependencies, retaining their BSD-style notices. |
| **Independent JPEG Group**, **libpng contributors**, **Jean-loup Gailly and Mark Adler / zlib** | JUCE's bundled image/compression support and original notices. |
| **HarfBuzz contributors** and **SheenBidi contributors** | JUCE text shaping and bidirectional text support; original MIT-style and Apache-2.0 notices. |

The complete distribution notices, including the original author/license texts,
are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). JUCE's own dependency list
also records platform-specific and optional modules.

## Branding

The **TTS logo** belongs to **The Tone Scientist** and is reused from
[NAM On Steroids](https://github.com/DLC86/NAM-Oversampler). It links to
[The Tone Scientist shop](https://shop.thetonescientist.com).
Branding retains its owner's rights; the software licenses do not grant a
separate trademark or logo license.

For the released implementation, use the version tag or
[Knobs+Switch](https://github.com/DLC86/NamParametricPlugin/tree/Knobs%2BSwitch)
and initialize its pinned submodules.
