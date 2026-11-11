#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

#include "NamParametricPluginProcessor.h"
#include "get_dsp.h"
#include "wavenet/hyperwavenet.h"

namespace {
using namparametric::dsp::ModelSize;
using namparametric::dsp::NamModelEngine;
using Json = nlohmann::json;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<float> Render(NamModelEngine& engine, int length, int block) {
  std::vector<float> input(static_cast<size_t>(length), 0.0f), output(input.size());
  for (int offset = 0; offset < length; offset += block)
    engine.Process(input.data() + offset, output.data() + offset, std::min(block, length - offset));
  Check(std::all_of(output.begin(), output.end(), [](float x) { return std::isfinite(x); }),
        "Control transition produced non-finite audio");
  return output;
}

void TestTransition(const std::filesystem::path& path, ModelSize size, double rate, int block) {
  NamModelEngine engine;
  std::string error;
  Check(engine.LoadModel(path.string(), error, size), error.c_str());
  engine.Reset(rate, block);
  auto values = engine.GetParameterValues();
  const auto before = Render(engine, 16000, block).back();
  NamModelEngine unchanged;
  Check(unchanged.LoadModel(path.string(), error, size), error.c_str());
  unchanged.Reset(rate, block);
  Render(unchanged, 16000, block);
  const auto baseline = Render(unchanged, 16000, block);
  // Move the knob and both switches together; every network still receives exact indices.
  values = {9.0f, 1.0f, 2.0f};
  Check(engine.SetParameterValues(values), "Valid control destination rejected");
  const auto changed = Render(engine, 16000, block);
  const float difference = std::abs(changed.back() - before);
  Check(difference > 1e-6f, "Fixture does not respond to the controls");
  Check(std::abs(changed.front() - baseline.front()) < difference * 0.001f,
        "Control update still jumps at the host block boundary");
  // Subtract an unchanged stream with identical resampler phase: polyphase
  // DC ripple is not a discontinuity introduced by a control update.
  float maxStep = std::abs(changed.front() - baseline.front());
  for (size_t i = 1; i < changed.size(); ++i)
    maxStep = std::max(maxStep,
                       std::abs((changed[i] - baseline[i]) - (changed[i - 1] - baseline[i - 1])));
  Check(maxStep < difference * 0.02f, "Transition has a discontinuity");
  if (rate == 48000 && block == 257) {
    // Spectrum of the step's derivative: a hard weight jump is an impulse,
    // whose magnitude is 'difference' at every frequency. Compare its upper
    // audio band with the actual rendered transition, including network history.
    double worstRatio = 0;
    for (int hz = 1000; hz <= 20000; hz += 1000) {
      double real = 0, imaginary = 0;
      float previous = before;
      for (size_t i = 0; i < changed.size(); ++i) {
        const double delta = changed[i] - previous;
        const double angle = 6.283185307179586 * hz * static_cast<double>(i) / rate;
        real += delta * std::cos(angle);
        imaginary -= delta * std::sin(angle);
        previous = changed[i];
      }
      worstRatio = std::max(worstRatio, std::hypot(real, imaginary) / difference);
    }
    Check(worstRatio < 0.01, "Transition retains excessive high-frequency step energy");
    std::cout << "Worst 1-20 kHz step spectrum vs hard jump: "
              << 20 * std::log10(std::max(worstRatio, 1e-12)) << " dB\n";
  }
  // A fresh stream must settle directly on the same destination, not glide from defaults.
  NamModelEngine settled;
  Check(settled.LoadModel(path.string(), error, size), error.c_str());
  Check(settled.SetParameterValues(values), "Restored controls rejected");
  settled.Reset(rate, block);
  Check(std::abs(Render(settled, 16000, block).back() - changed.back()) < 1e-6f,
        "Fade did not reach the exact destination model");

  // Rapid retargets must queue the latest complete vector, rather than restarting
  // a half-finished fade (which would cause another discontinuity).
  for (int i = 0; i < 19; ++i) {
    values = {static_cast<float>(i % 11), static_cast<float>(i % 2), static_cast<float>(i % 3)};
    Check(engine.SetParameterValues(values), "Retarget rejected");
    Render(engine, 37, block);
  }
  const auto latest = engine.GetParameterValues();
  const auto after = Render(engine, 16000, block).back();
  Check(settled.SetParameterValues(latest), "Final controls rejected");
  settled.Reset(rate, block);
  Check(std::abs(Render(settled, 16000, block).back() - after) < 1e-6f,
        "Rapid retarget lost the latest destination");
  auto invalid = latest;
  invalid[1] = 0.5f;
  Check(!engine.SetParameterValues(invalid) && engine.GetParameterValues() == latest,
        "Fractional switch destination was accepted");
  invalid = latest;
  invalid[0] = std::numeric_limits<float>::quiet_NaN();
  Check(!engine.SetParameterValues(invalid), "NaN destination was accepted");

  std::cout << path.filename() << " size=" << (size == ModelSize::Full ? "Full" : "Lite")
            << " rate=" << rate << " block=" << block
            << " maximum step/jump=" << maxStep / difference << '\n';
}

void Set(NamParametricPluginAudioProcessor& processor, const char* id, float plain) {
  auto* parameter = processor.mValueTree.getParameter(id);
  parameter->setValueNotifyingHost(parameter->convertTo0to1(plain));
}

void TestEco(const std::filesystem::path& path, ModelSize size, double rate, int block) {
  using namparametric::dsp::ControlTransition;
  NamModelEngine eco, reference;
  std::string error;
  eco.SetControlTransition(ControlTransition::Eco);
  reference.SetControlTransition(ControlTransition::Eco);
  Check(eco.LoadModel(path.string(), error, size), error.c_str());
  Check(reference.LoadModel(path.string(), error, size), error.c_str());
  eco.Reset(rate, block);
  const float before = Render(eco, 16000, block).back();
  const std::vector<float> destination{9, 1, 2};
  Check(eco.SetParameterValues(destination), "Eco destination rejected");
  const auto moved = Render(eco, 16000, block);
  Check(reference.SetParameterValues(destination), "Reference destination rejected");
  reference.Reset(rate, block);
  const auto expected = Render(reference, 32000, block).back();
  Check(std::abs(moved.back() - expected) < 1e-6f, "Eco did not reach the unmodified model");
  const float difference = std::abs(expected - before);
  if (rate == 48000) {
    float maxStep = std::abs(moved.front() - before);
    for (size_t i = 1; i < moved.size(); ++i)
      maxStep = std::max(maxStep, std::abs(moved[i] - moved[i - 1]));
    Check(maxStep < difference * 0.03f, "Eco left a hard step on the DC control fixture");
    std::cout << "Eco rate=" << rate << " block=" << block
              << " maximum step/hard jump=" << maxStep / difference << '\n';
  }
  // Switching to a sleeping standby must not expose its stale history.
  eco.SetControlTransition(ControlTransition::Crossfade);
  const auto switched = Render(eco, 32000, block);
  Check(std::abs(switched.back() - expected) < 1e-6f, "Mode change lost the current controls");
  eco.SetParameterValues(std::vector<float>{1, 0, 1});
  Render(eco, 23, block);
  eco.SetControlTransition(ControlTransition::Eco);
  for (int i = 0; i < 19; ++i) {
    eco.SetParameterValues(std::vector<float>{static_cast<float>(i % 11), static_cast<float>(i % 2),
                                              static_cast<float>(i % 3)});
    Render(eco, 37, block);
  }
  const auto latest = eco.GetParameterValues();
  const auto settled = Render(eco, 32000, block).back();
  reference.SetParameterValues(latest);
  reference.Reset(rate, block);
  Check(std::abs(Render(reference, 32000, block).back() - settled) < 1e-6f,
        "Eco rapid moves/mode change lost the final control vector");
}

// Real audio-block regression: a second knob/switch vector is accepted immediately
// but MUST NOT recondition weights until 20 ms of model audio has been rendered.
// Both engines render identical input/history; one receives the pending request,
// the other retains the currently applied controls.
void TestEcoRateLimit(const std::filesystem::path& path, ModelSize size, int block) {
  using namparametric::dsp::ControlTransition;
  NamModelEngine limited, reference, settled;
  std::string error;
  for (auto* engine : {&limited, &reference, &settled}) {
    engine->SetControlTransition(ControlTransition::Eco);
    Check(engine->LoadModel(path.string(), error, size), error.c_str());
    engine->Reset(48000, block);
  }
  Render(limited, 16000, block);
  Render(reference, 16000, block);
  const std::vector<float> first{9.0f, 1.0f, 2.0f};
  const std::vector<float> last{2.0f, 0.0f, 1.0f};
  Check(limited.SetParameterValues(first) && reference.SetParameterValues(first),
        "Eco's initial control change was rejected");
  Render(limited, 37, block);
  Render(reference, 37, block);
  Check(limited.SetParameterValues(last), "Eco pending target was rejected");
  Check(limited.GetParameterValues() == last,
        "Eco should expose the latest requested value even during cooldown");
  // 37 + 864 = 901 model samples, still shorter than 20 ms (960 samples).
  // Render at several host block sizes to catch block-time vs wall-clock bugs.
  const auto throttled = Render(limited, 864, block);
  const auto unchanged = Render(reference, 864, block);
  for (size_t i = 0; i < throttled.size(); ++i)
    Check(std::abs(throttled[i] - unchanged[i]) < 1e-7f,
          "Eco recalculated weights before its 20 ms cooldown elapsed");
  // Only the last requested target is applied once the model-sample clock
  // permits it; after the 5 ms correction tail, audio must match a fresh model.
  const float output = Render(limited, 24000, block).back();
  Check(settled.SetParameterValues(last), "Final Eco destination rejected");
  settled.Reset(48000, block);
  const float expected = Render(settled, 24000, block).back();
  Check(std::abs(output - expected) < 1e-6f,
        "Eco 20 ms rate limiting did not settle on the last destination");
}

// A real zero-input regression: DC output can change with controls even when
// every input sample is exactly 0. Compare high-frequency steps with the
// unmodified single-network hypernetwork (instantaneous weights), and also
// report the continuously running two-network Crossfade result for context.
void TestSilentControlSweeps(const std::filesystem::path& path, const Json& hyper) {
  using namparametric::dsp::ControlTransition;
  NamModelEngine eco, crossfade;
  std::string error;
  eco.SetControlTransition(ControlTransition::Eco);
  crossfade.SetControlTransition(ControlTransition::Crossfade);
  Check(eco.LoadModel(path.string(), error), error.c_str());
  Check(crossfade.LoadModel(path.string(), error), error.c_str());
  eco.Reset(48000, 257);
  crossfade.Reset(48000, 257);
  auto raw = nam::get_dsp(hyper);
  raw->Reset(48000, 257);
  auto rawSilent = [&](int frames) {
    std::vector<float> input(static_cast<size_t>(frames), 0.0f), out(input.size());
    for (int i = 0; i < frames; i += 257) {
      auto* in = input.data() + i;
      auto* dst = out.data() + i;
      raw->process(&in, &dst, std::min(257, frames - i));
    }
    return out;
  };
  // Keep the actual last steady-state sample as the derivative reference.
  // These models can output a nonzero DC bias at exactly zero digital input:
  // comparing the first sweep sample against literal zero would instead
  // measure the *DC level*, not a control-change click, in every mode.
  const float ecoPreviousBefore = Render(eco, 16000, 257).back();
  const float crossfadePreviousBefore = Render(crossfade, 16000, 257).back();
  const float rawPreviousBefore = rawSilent(16000).back();
  double ecoPeak = 0.0, rawPeak = 0.0, crossfadePeak = 0.0;
  double ecoPower = 0.0, rawPower = 0.0;
  float ecoPrevious = ecoPreviousBefore, rawPrevious = rawPreviousBefore;
  float crossfadePrevious = crossfadePreviousBefore;
  for (const auto values : {std::vector<float>{9, 1, 2}, {1, 0, 0},
                            {10, 0, 2}, {2, 1, 1}}) {
    Check(eco.SetParameterValues(values) && crossfade.SetParameterValues(values),
          "Silent-sweep controls rejected");
    dynamic_cast<nam::IParametricControl&>(*raw).SetParams(values);
    // One full move and one settle interval, both with exact digital silence.
    const auto ecoOut = Render(eco, 4800, 257);
    const auto rawOut = rawSilent(4800);
    const auto fadeOut = Render(crossfade, 4800, 257);
    for (size_t i = 0; i < ecoOut.size(); ++i) {
      const double ecoDiff = static_cast<double>(ecoOut[i]) - ecoPrevious;
      const double rawDiff = static_cast<double>(rawOut[i]) - rawPrevious;
      const double fadeDiff = static_cast<double>(fadeOut[i]) - crossfadePrevious;
      ecoPrevious = ecoOut[i];
      rawPrevious = rawOut[i];
      crossfadePrevious = fadeOut[i];
      ecoPeak = std::max(ecoPeak, std::abs(ecoDiff));
      rawPeak = std::max(rawPeak, std::abs(rawDiff));
      crossfadePeak = std::max(crossfadePeak, std::abs(fadeDiff));
      ecoPower += ecoDiff * ecoDiff;
      rawPower += rawDiff * rawDiff;
    }
  }
  std::cout << "Zero-input knob sweeps: weight-smoothed Eco max d/dt=" << ecoPeak
            << " raw=" << rawPeak << " Crossfade=" << crossfadePeak
            << " high-frequency step RMS ratio="
            << std::sqrt(ecoPower / std::max(1e-30, rawPower)) << '\n';
  // The synthetic fixture has a nonzero control-dependent DC bias. Smoothing
  // should attenuate its impulsive derivative without an output noise gate.
  Check(rawPeak > 1e-8, "Silent-sweep fixture does not respond at zero input");
  Check(ecoPeak < rawPeak * 0.70,
        "Weight interpolation did not sufficiently reduce zero-input step peaks");
}

void TestEcoUnaffectedSignal(const std::filesystem::path& directory, Json hyper) {
  // Zero the hypernetwork, leaving the audio base intact. Knob movement now
  // changes no weights: the de-click must not invent a transient on ordinary audio.
  auto config = nam::wavenet::create_hyperwavenet_config(hyper.at("config"), 48000);
  const auto& typed = dynamic_cast<const nam::wavenet::HyperWaveNetConfig&>(*config);
  const auto count = nam::Hypernetwork::SerializedStateCount(typed.hypernet, typed.params);
  for (size_t i = hyper["weights"].size() - count; i < hyper["weights"].size(); ++i)
    hyper["weights"][i] = 0.0;
  const auto path = directory / "eco_unchanged.nam";
  {
    std::ofstream stream(path);
    stream << hyper;
  }
  NamModelEngine eco;
  eco.SetControlTransition(namparametric::dsp::ControlTransition::Eco);
  std::string error;
  Check(eco.LoadModel(path.string(), error), error.c_str());
  eco.Reset(48000, 257);
  auto raw = nam::get_dsp(hyper);
  raw->Reset(48000, 257);
  float input[257]{}, actual[257]{}, expected[257]{};
  double maximum = 0;
  for (int block = 0; block < 200; ++block) {
    if (block > 20 && block % 2 == 0)
      eco.SetParameterValues(std::vector<float>{static_cast<float>(block % 11),
                                                static_cast<float>(block % 2),
                                                static_cast<float>(block % 3)});
    for (int i = 0; i < 257; ++i) {
      const double t = static_cast<double>(block * 257 + i) / 48000;
      input[i] = static_cast<float>(0.3 * std::sin(t * 6.283185307179586 * 997) +
                                    0.2 * std::sin(t * 6.283185307179586 * 12000));
    }
    auto* in = input;
    auto* out = expected;
    raw->process(&in, &out, 257);
    eco.Process(input, actual, 257);
    for (int i = 0; i < 257; ++i)
      maximum = std::max(maximum, std::abs(static_cast<double>(actual[i]) - expected[i]));
  }
  Check(maximum < 1e-6, "Eco modified audio when the controls had no audible effect");
  std::cout << "Eco unaffected signal max error=" << maximum << '\n';
}

float ProcessSilence(NamParametricPluginAudioProcessor& processor, int blocks = 1) {
  juce::AudioBuffer<float> audio(2, 257);
  juce::MidiBuffer midi;
  for (int i = 0; i < blocks; ++i) {
    audio.clear();
    processor.processBlock(audio, midi);
  }
  return audio.getSample(0, 256);
}

void TestLiveCalibration(const std::filesystem::path& path) {
  NamParametricPluginAudioProcessor processor;
  processor.prepareToPlay(48000, 257);
  Set(processor, "calibrateInput", 0);
  Set(processor, "outputMode", 2);
  processor.LoadModelAsync(juce::File(juce::String(path.string())));
  const auto deadline = juce::Time::getMillisecondCounterHiRes() + 15000;
  while (!processor.GetStatusText().startsWith("Loaded model:")) {
    Check(juce::Time::getMillisecondCounterHiRes() < deadline, "Model load timed out");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  }
  const float original = ProcessSilence(processor, 100);
  Set(processor, "inputCalibration", 18);
  Check(std::abs(ProcessSilence(processor) - original) < 1e-7f,
        "Input calibration unexpectedly changed the output trim");
  // Like NAM Oversampler, an explicit output-mode change takes a new reference.
  Set(processor, "outputMode", 0);
  const float raw = ProcessSilence(processor);
  Set(processor, "outputMode", 2);
  const float rereferenced = ProcessSilence(processor);
  Check(std::abs(original - raw * std::pow(10.0f, -6.0f / 20.0f)) < 1e-6f &&
            std::abs(rereferenced - raw * std::pow(10.0f, -12.0f / 20.0f)) < 1e-6f,
        "Calibrated output does not match NAM Oversampler");
  // Input correction itself remains live and retains the reference implementation's sign.
  NamModelEngine engine;
  std::string error;
  Check(engine.LoadModel(path.string(), error), error.c_str());
  Check(engine.GetInputCalibrationGainDb(12, true) == -6 &&
            engine.GetInputCalibrationGainDb(18, true) == 0 &&
            engine.GetInputCalibrationGainDb(24, true) == 6,
        "Input calibration sign or live response is incorrect");
  Check(processor.mValueTree.getRawParameterValue("controlTransition")->load() == 0.0f,
        "New processors must start in Eco");
  Set(processor, "controlTransition", 1);
  juce::MemoryBlock state;
  processor.getStateInformation(state);
  NamParametricPluginAudioProcessor restored;
  restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
  Check(restored.mValueTree.getRawParameterValue("controlTransition")->load() == 1.0f,
        "Crossfade selection was not recalled");
  auto xml =
      juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
  auto legacy = juce::ValueTree::fromXml(*xml);
  legacy.removeChild(legacy.getChildWithProperty("id", "controlTransition"), nullptr);
  juce::MemoryBlock oldState;
  auto oldXml = legacy.createXml();
  juce::AudioProcessor::copyXmlToBinary(*oldXml, oldState);
  restored.setStateInformation(oldState.getData(), static_cast<int>(oldState.getSize()));
  Check(restored.mValueTree.getRawParameterValue("controlTransition")->load() == 0.0f,
        "Sessions without a saved smoothing mode must migrate to Eco");
}
}  // namespace

void TestControlTransitions(const std::filesystem::path& directory, const Json& hyper,
                            const Json& a2) {
  for (const bool nested : {false, true}) {
    const auto path = directory / (nested ? "transition_a2.nam" : "transition_hyper.nam");
    {
      std::ofstream stream(path);
      stream << (nested ? a2 : hyper);
    }
    for (const auto size : {ModelSize::Full, ModelSize::Lite}) {
      if (!nested && size == ModelSize::Lite) continue;
      for (const int block : {37, 257, 2048}) TestTransition(path, size, 48000, block);
      TestTransition(path, size, 44100, 257);
      TestTransition(path, size, 96000, 257);
      for (const int block : {37, 257, 2048}) TestEco(path, size, 48000, block);
      for (const int block : {37, 257, 2048}) TestEcoRateLimit(path, size, block);
      TestEco(path, size, 44100, 257);
      TestEco(path, size, 96000, 257);
    }
    TestLiveCalibration(path);
    // Extension is a label, not a different container. Both legacy .nam and
    // .pnam must preserve the complete model, controls and metadata on load.
    auto pnam = path;
    pnam.replace_extension(".pnam");
    std::filesystem::copy_file(path, pnam, std::filesystem::copy_options::overwrite_existing);
    for (const auto size : {ModelSize::Full, ModelSize::Lite}) {
      NamModelEngine legacy, renamed;
      std::string error;
      Check(legacy.LoadModel(path.string(), error, size), error.c_str());
      Check(renamed.LoadModel(pnam.string(), error, size), error.c_str());
      Check(*legacy.GetExportSource() == *renamed.GetExportSource(),
            "PNAM load changed model data or metadata");
      legacy.Reset(48000, 257);
      renamed.Reset(48000, 257);
      Check(Render(legacy, 8192, 257) == Render(renamed, 8192, 257),
            "PNAM load changed the audio response");
    }
  }
  TestEcoUnaffectedSignal(directory, hyper);
  TestSilentControlSweeps(directory / "transition_hyper.nam", hyper);
  // Cost probe uses the same HyperWaveNet, audio and block size for raw core
  // and plugin smoothing. Report, rather than assert, machine-dependent timing.
  auto raw = nam::get_dsp(hyper);
  raw->Reset(48000, 257);
  NamModelEngine engine;
  std::string error;
  Check(engine.LoadModel((directory / "transition_hyper.nam").string(), error), error.c_str());
  engine.Reset(48000, 257);
  NamModelEngine eco;
  eco.SetControlTransition(namparametric::dsp::ControlTransition::Eco);
  Check(eco.LoadModel((directory / "transition_hyper.nam").string(), error), error.c_str());
  eco.Reset(48000, 257);
  float input[257]{}, output[257]{};
  const auto measure = [&](int mode, bool moving = false) {
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 400; ++i) {
      if (moving && i % 3 == 0) {
        const std::vector<float> values{static_cast<float>(i % 11), static_cast<float>(i % 2),
                                        static_cast<float>(i % 3)};
        if (mode == 0)
          dynamic_cast<nam::IParametricControl&>(*raw).SetParams(values);
        else if (mode == 2)
          eco.SetParameterValues(values);
        else
          engine.SetParameterValues(values);
      }
      if (mode == 2)
        eco.Process(input, output, 257);
      else if (mode == 1)
        engine.Process(input, output, 257);
      else {
        float* in = input;
        float* out = output;
        raw->process(&in, &out, 257);
      }
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
  };
  const auto rawMs = measure(0);
  const auto smoothMs = measure(1);
  const auto ecoMs = measure(2);
  std::cout << "400 x 257 samples, raw=" << rawMs << " ms, smooth=" << smoothMs
            << " ms, eco=" << ecoMs << " ms; crossfade/raw=" << smoothMs / rawMs
            << ", eco/raw=" << ecoMs / rawMs << '\n';
  const auto movingRaw = measure(0, true);
  const auto movingEco = measure(2, true);
  std::cout << "Moving controls: raw=" << movingRaw << " ms, eco=" << movingEco
            << " ms, eco/raw=" << movingEco / movingRaw << '\n';
  // Metadata parity with NAM Oversampler: case-insensitive gear and old return keys.
  for (const char* key : {"return_level", "return_level_dbu"}) {
    auto legacy = hyper;
    legacy["metadata"].erase("output_level_dbu");
    legacy["metadata"][key] = 6.0;
    legacy["metadata"]["gear_type"] = "AMP";
    const auto path = directory / "legacy_calibration.nam";
    {
      std::ofstream stream(path);
      stream << legacy;
    }
    NamModelEngine legacyEngine;
    std::string legacyError;
    Check(legacyEngine.LoadModel(path.string(), legacyError), legacyError.c_str());
    Check(legacyEngine.GetOutputCalibrationGainDb(12, namparametric::dsp::OutputMode::Auto) == 6 &&
              legacyEngine.GetOutputCalibrationGainDb(
                  12, namparametric::dsp::OutputMode::Calibrated) == -6,
          "Legacy metadata differs from NAM Oversampler");
  }
}
