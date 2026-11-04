# Parametric DSP optimization

HyperA2 now uses the specialized A2 audio backend for both Full and Lite.
Control changes update weights in allocated storage without discarding audio
history. The strict A2 shape check retains the generic fallback for other
HyperWaveNet architectures. Calibration, Eco de-click, crossfade mode, metadata,
resampling and static export remain in the existing plugin engine.

Small Full blocks use an SSE2 four-frame kernel adapted from NeuralAudio's
multi-frame weight reuse; larger blocks keep Eigen, which measured faster on
this compiler. Lite uses the specialized unrolled 3 × 3 kernel. No AVX2 requirement,
extra block latency, architecture change or retraining is introduced.

## Reproducible benchmark

CPU: 12th Gen Intel(R) Core(TM) i7-12700H. Windows x64, MSVC 19.51, Release /O2. Baseline: plugin
`03b423f336dbfb4f86308e65445f468e2e00ad46` with core
`fddcbc91e74ba79066a43c3b73cc690073529232`.

Synthetic low-rank HyperA2 fixture, 48 kHz, Eco control transition. Each result is
the median of three alternating baseline/optimized runs, with 200 warmup blocks
and 1000 timed blocks per case. Moving controls are updated every block. Loading,
prewarm and audio-device overhead are excluded. Results describe this machine and
fixture; model hypernetwork size, host and CPU will affect actual savings.

| Model | Frames | Controls | Baseline µs/block | Optimized µs/block | CPU time reduction |
| --- | ---: | --- | ---: | ---: | ---: |
| Full | 32 | Fixed | 71.8 | 55.6 | 22.5% |
| Full | 32 | Moving | 96.1 | 65.2 | 32.2% |
| Full | 64 | Fixed | 121.9 | 102.7 | 15.7% |
| Full | 64 | Moving | 153.3 | 119.2 | 22.2% |
| Full | 128 | Fixed | 233.1 | 195.4 | 16.2% |
| Full | 128 | Moving | 267.1 | 213.3 | 20.2% |
| Full | 256 | Fixed | 480.1 | 380.1 | 20.8% |
| Full | 256 | Moving | 494.1 | 410.6 | 16.9% |
| Lite | 32 | Fixed | 44.2 | 15.4 | 65.1% |
| Lite | 32 | Moving | 49.7 | 18.5 | 62.7% |
| Lite | 64 | Fixed | 77.5 | 29.5 | 62.0% |
| Lite | 64 | Moving | 85.3 | 33.6 | 60.7% |
| Lite | 128 | Fixed | 143.0 | 58.6 | 59.0% |
| Lite | 128 | Moving | 146.5 | 61.9 | 57.8% |
| Lite | 256 | Fixed | 260.8 | 117.5 | 55.0% |
| Lite | 256 | Moving | 265.6 | 124.6 | 53.1% |

Raw aggregate values, including p99, are in [pnam-benchmark-x64.json](pnam-benchmark-x64.json).
Build `pnam_benchmark` and pass a model path to print JSON timings. The independent
`Tests/Runtime` CMake project builds it without JUCE. `pnam_fast_path_test` compares
generic and optimized audio under changing controls and variable/odd block sizes;
the golden engine tests compare against PyTorch. The maximum observed audio error
in the automation test was 2.43e-8 (absolute sample amplitude).

Windows plugin regressions also cover calibration, Eco/crossfade control changes,
export and every 3 × 2 × 5 combination of independent switches mixed with knobs.
Portable CI additionally checks Linux/macOS and ARM64; these are correctness tests,
not claims about the real-time performance of a particular ARM board.
