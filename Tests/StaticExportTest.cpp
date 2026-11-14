#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <juce_audio_devices/juce_audio_devices.h>

#include "NamParametricPluginProcessor.h"
#include "UI/ModelBarComponent.h"
#include "get_dsp.h"
#include "slimmable.h"
#include "wavenet/a2_fast.h"
#include "wavenet/hyperwavenet.h"

void TestControlTransitions(const std::filesystem::path& directory, const nlohmann::json& hyper,
                            const nlohmann::json& a2);

namespace {
using Json = nlohmann::json;
using namparametric::dsp::ModelSize;
using namparametric::dsp::NamModelEngine;
using Processor = NamParametricPluginAudioProcessor;

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

// Self-contained, deterministic models: no trainer, Python or downloaded model is
// needed to run the export regression on either platform.
Json MakeHyper(int channels, bool lowRank) {
  const size_t baseCount = channels == 3 ? 1871 : 12146;
  Json activations = Json::array();
  for (int i = 0; i < 23; ++i)
    activations.push_back({{"type", "LeakyReLU"}, {"negative_slope", 0.01}});
  Json layer = {{"input_size", 1},
                {"condition_size", 1},
                {"channels", channels},
                {"bottleneck", channels},
                {"kernel_sizes", nam::wavenet::a2_fast::kKernelSizes},
                {"dilations", nam::wavenet::a2_fast::kDilations},
                {"activation", activations},
                {"layer1x1", {{"active", true}, {"groups", 1}}},
                {"head", {{"out_channels", 1}, {"kernel_size", 16}, {"bias", true}}}};
  Json specs = Json::array(
      {{{"name", "Gain"}, {"type", "continuous"}, {"min", 0}, {"max", 10}, {"default", 2}},
       {{"name", "Boost"},
        {"type", "switch"},
        {"min", 0},
        {"max", 1},
        {"default", 0},
        {"enum_names", {"Off", "On"}}},
       {{"name", "Mode"},
        {"type", "switch"},
        {"min", 0},
        {"max", 2},
        {"default", 0},
        {"enum_names", {"A", "B", "C"}}}});
  Json target = {{"name", "_rechannel.weight"},
                 {"numel", channels},
                 {"export_offset", 0},
                 {"mode", lowRank ? "low_rank" : "full"}};
  if (lowRank) {
    target["rank"] = 2;
    target["out_features"] = channels;
    target["rest_features"] = 1;
  }
  Json hypernet = {{"mode", lowRank ? "low_rank" : "full"},
                   {"rank", 2},
                   {"hidden_sizes", {4}},
                   {"activation", "Tanh"},
                   {"delta_map", Json::array({target,
                                              {{"name", "head.bias"},
                                               {"mode", "full"},
                                               {"numel", 1},
                                               {"export_offset", baseCount - 2}}})}};
  Json config = {{"layers", Json::array({layer})},
                 {"head", nullptr},
                 {"head_scale", 0.75},
                 {"params", specs},
                 {"hypernet", hypernet}};
  auto parsed = nam::wavenet::create_hyperwavenet_config(config, 48000);
  const auto& hyper = dynamic_cast<const nam::wavenet::HyperWaveNetConfig&>(*parsed);
  const auto count = nam::Hypernetwork::SerializedStateCount(hyper.hypernet, hyper.params);
  std::vector<float> weights(baseCount + count);
  for (size_t i = 0; i < weights.size(); ++i)
    weights[i] = 0.025f * static_cast<float>(std::sin(static_cast<double>(i) * 0.37));
  weights[baseCount - 1] = 0.75f;
  return {{"version", "1.0.0"},
          {"architecture", "HyperWaveNet"},
          {"config", config},
          {"sample_rate", 48000},
          {"weights", weights},
          {"metadata",
           {{"name", "Original pedal"},
            {"input_level_dbu", 18.0},
            {"output_level_dbu", 6.0},
            {"loudness", -24.0},
            {"other_custom", {{"preserve", true}}}}}};
}

Json MakeA2(bool lowRank) {
  const auto lite = MakeHyper(3, lowRank);
  const auto full = MakeHyper(8, lowRank);
  Json children = Json::array();
  size_t offset = 0;
  for (const auto* child : {&lite, &full}) {
    const bool isLite = offset == 0;
    children.push_back({{"name", isLite ? "lite" : "full"},
                        {"architecture", "HyperWaveNet"},
                        {"max_value", isLite ? 0.5 : 1.0},
                        {"weight_offset", offset},
                        {"weight_count", child->at("weights").size()},
                        {"base_weight_count", isLite ? 1871 : 12146},
                        {"config", child->at("config")}});
    offset += child->at("weights").size();
  }
  Json result = lite;
  result["architecture"] = "HyperA2";
  result["config"] = {
      {"params", lite["config"]["params"]}, {"submodels", children}, {"default_size", "lite"}};
  for (const auto& weight : full["weights"]) result["weights"].push_back(weight);
  return result;
}

std::vector<float> Render(nam::DSP& model) {
  model.SetPrewarmOnReset(false);
  model.Reset(48000, 257);
  std::vector<float> input(8192), output(input.size());
  for (size_t i = 0; i < input.size(); ++i)
    input[i] = static_cast<float>(0.1 * std::sin(i * 0.028) + 0.03 * std::cos(i * 0.13));
  for (size_t offset = 0; offset < input.size(); offset += 257) {
    auto* in = input.data() + offset;
    auto* out = output.data() + offset;
    model.process(&in, &out, static_cast<int>(std::min<size_t>(257, input.size() - offset)));
  }
  return output;
}

void Compare(const std::vector<float>& actual, const std::vector<float>& expected) {
  double maximum = 0;
  for (size_t i = 0; i < actual.size(); ++i) {
    Check(std::isfinite(actual[i]), "Non-finite static output");
    maximum = std::max(maximum, std::abs(static_cast<double>(actual[i]) - expected[i]));
  }
  Check(maximum < 2e-6, "Static export audio differs: " + std::to_string(maximum));
  std::cout << "Static export audio max_error=" << maximum << '\n';
}

void Write(const std::filesystem::path& path, const Json& model) {
  std::ofstream file(path);
  file << model.dump();
  Check(file.good(), "Could not write fixture");
}

void TestBaking(const std::filesystem::path& directory) {
  for (const bool lowRank : {false, true}) {
    for (const auto values : {std::vector<float>{2, 0, 0}, std::vector<float>{6.5f, 1, 2}}) {
      const auto source = MakeA2(lowRank);
      const auto baked = NamModelEngine::BakeStaticModel(source, values, "Pedal.nam");
      Check(baked["architecture"] == "SlimmableContainer" && baked["weights"].empty(),
            "HyperA2 must export a stock container");
      const auto& children = baked["config"]["submodels"];
      Check(children.size() == 2 && children[0]["max_value"] == 0.5 &&
                children[1]["max_value"] == 1.0,
            "Both sizes must be present in Lite/Full order");
      auto container = nam::get_dsp(baked);
      auto* slimmable = dynamic_cast<nam::SlimmableModel*>(container.get());
      Check(slimmable != nullptr, "Stock loader did not expose size selection");
      for (size_t index = 0; index < 2; ++index) {
        const auto hyper = MakeHyper(index == 0 ? 3 : 8, lowRank);
        auto live = nam::get_dsp(hyper);
        dynamic_cast<nam::IParametricControl&>(*live).SetParams(values);
        const auto expected = Render(*live);
        const auto& child = children[index]["model"];
        Check(child["architecture"] == "WaveNet" && !child["config"].contains("params") &&
                  !child["config"].contains("hypernet"),
              "Export still depends on an hypernetwork");
        auto fixed = nam::get_dsp(child);
        Check(dynamic_cast<nam::IParametricControl*>(fixed.get()) == nullptr,
              "Export is parametric");
        Compare(Render(*fixed), expected);
        slimmable->SetSlimmableSize(index == 0 ? 0.0 : 1.0);
        Compare(Render(*container), expected);
        const auto standalone = NamModelEngine::BakeStaticModel(hyper, values, "Hyper.nam");
        Compare(Render(*nam::get_dsp(standalone)), expected);
        Check(child["weights"] == standalone["weights"],
              "A2 branch weights differ from HyperWaveNet baking");
        Check(child["weights"].size() == (index == 0 ? 1871u : 12146u) &&
                  child["weights"].back() == 0.75f,
              "Head scale or weight layout changed");
      }
      const auto& metadata = baked["metadata"];
      const auto& provenance = metadata["nam_parametric_plugin"]["static_export"];
      Check(metadata["other_custom"]["preserve"] == true && metadata["input_level_dbu"] == 18.0 &&
                metadata["output_level_dbu"] == 6.0 && provenance["source_file"] == "Pedal.nam" &&
                provenance["variants"] == Json({"lite", "full"}),
            "Original/provenance metadata lost");
      Check(provenance["controls"][0]["name"] == "Gain" &&
                provenance["controls"][0]["value"] == values[0] &&
                provenance["controls"][1]["label"] == (values[1] == 0 ? "Off" : "On") &&
                provenance["controls"][2]["value"] == values[2],
            "Control metadata differs from baking");
      const auto path = directory / (lowRank ? "static-low-rank.nam" : "static-full.nam");
      Write(path, baked);
      Write(directory / (lowRank ? "source-low-rank.nam" : "source-full.nam"), source);
      for (const auto size : {ModelSize::Lite, ModelSize::Full}) {
        NamModelEngine engine;
        std::string error;
        Check(engine.LoadModel(path.string(), error, size), error);
        Check(engine.SupportsSizeSelection() && engine.GetModelSize() == size &&
                  engine.GetParameterInfos().empty(),
              "Plugin cannot reload both static sizes");
      }
    }
  }
  auto source = MakeA2(true);
  auto generic = MakeHyper(3, true);
  generic["config"]["layers"][0]["dilations"][0] = 2;  // Exercise the non-A2 WaveNet loader.
  const std::vector<float> genericValues{6.5f, 1, 2};
  auto genericLive = nam::get_dsp(generic);
  dynamic_cast<nam::IParametricControl&>(*genericLive).SetParams(genericValues);
  const auto genericStatic = NamModelEngine::BakeStaticModel(generic, genericValues, "Generic.nam");
  Compare(Render(*nam::get_dsp(genericStatic)), Render(*genericLive));
  for (const auto values : {std::vector<float>{1}, std::vector<float>{1, 0.5f, 0},
                            std::vector<float>{1, 0, 3}, std::vector<float>{11, 0, 0},
                            std::vector<float>{std::numeric_limits<float>::quiet_NaN(), 0, 0}}) {
    bool rejected = false;
    try {
      NamModelEngine::BakeStaticModel(source, values, "Invalid.nam");
    } catch (const std::exception&) {
      rejected = true;
    }
    Check(rejected, "Invalid export controls accepted");
  }
}

void WaitForLoad(Processor& processor) {
  const auto deadline = juce::Time::getMillisecondCounterHiRes() + 30000;
  while (!processor.HasModelLoaded() || !processor.CanExportStaticModel()) {
    Check(juce::Time::getMillisecondCounterHiRes() < deadline, "Model load timed out");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  }
}


void WaitForStaticModel(Processor& processor) {
  const auto deadline = juce::Time::getMillisecondCounterHiRes() + 30000;
  while (!processor.HasModelLoaded()) {
    Check(juce::Time::getMillisecondCounterHiRes() < deadline, "Static model load timed out");
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  }
}

std::vector<float> RenderStaticProcessor(Processor& processor) {
  constexpr int blockSize = 257;
  constexpr int length = 8224;
  processor.prepareToPlay(48000.0, blockSize);
  std::vector<float> samples;
  samples.reserve(length);
  juce::MidiBuffer midi;
  juce::AudioBuffer<float> buffer(2, blockSize);
  for (int offset = 0; offset < length; offset += blockSize) {
    buffer.clear();
    auto* input = buffer.getWritePointer(0);
    for (int i = 0; i < blockSize; ++i) {
      const auto t = static_cast<double>(offset + i);
      input[i] = static_cast<float>(0.10 * std::sin(t * 0.028) +
                                    0.03 * std::cos(t * 0.13));
    }
    processor.processBlock(buffer, midi);
    samples.insert(samples.end(), buffer.getReadPointer(0), buffer.getReadPointer(0) + blockSize);
  }
  return samples;
}

void CheckOutputScale(const std::vector<float>& reference, const std::vector<float>& actual,
                      const double db, const char* message) {
  Check(reference.size() == actual.size(), "Static rendering size mismatch");
  const double multiplier = std::pow(10.0, db / 20.0);
  double maxError = 0.0;
  double maxSignal = 0.0;
  for (size_t i = 0; i < actual.size(); ++i) {
    const double expected = reference[i] * multiplier;
    maxError = std::max(maxError, std::abs(static_cast<double>(actual[i]) - expected));
    maxSignal = std::max(maxSignal, std::abs(expected));
  }
  Check(maxSignal > 1.0e-6, "Static calibration fixture produced silence");
  Check(maxError < std::max(1.0e-6, maxSignal * 2.0e-4), message);
}

void TestStaticOutputModeStability(const std::filesystem::path& directory) {
  using namparametric::dsp::OutputMode;
  auto staticModel = NamModelEngine::BakeStaticModel(
      MakeA2(true), std::vector<float>{2.0f, 0.0f, 0.0f}, "CalibrationFixture.pnam");
  // Real-world SlimmableContainer layout: calibration and gear type are
  // ONLY on the outer model. Lite/Full store their own loudness.
  staticModel["metadata"]["gear_type"] = "amp";
  staticModel["metadata"]["loudness"] = -20.0; // fallback, not the active loudness
  for (size_t index = 0; index < 2; ++index) {
    auto& metadata = staticModel["config"]["submodels"][index]["model"]["metadata"];
    metadata.erase("input_level_dbu");
    metadata.erase("output_level_dbu");
    metadata.erase("gear_type");
    metadata["loudness"] = index == 0 ? -24.0 : -22.0;
  }
  const auto path = directory / "calibration_modes_static.nam";
  Write(path, staticModel);

  for (const auto size : {ModelSize::Full, ModelSize::Lite}) {
    Processor processor;
    processor.prepareToPlay(48000.0, 257);
    auto* mode = processor.mValueTree.getParameter("outputMode");
    auto* inputCalibration = processor.mValueTree.getParameter("inputCalibration");
    auto* outputGain = processor.mValueTree.getParameter("outputGainDb");
    auto* calibrateInput = processor.mValueTree.getParameter("calibrateInput");
    Check(mode && inputCalibration && outputGain && calibrateInput, "Missing gain parameters");
    calibrateInput->setValueNotifyingHost(0.0f);
    inputCalibration->setValueNotifyingHost(inputCalibration->convertTo0to1(12.0f));
    processor.LoadModelAsync(juce::File(juce::String(path.string())));
    WaitForStaticModel(processor);
    if (size == ModelSize::Lite) {
      processor.SetModelSize(ModelSize::Lite);
      const auto deadline = juce::Time::getMillisecondCounterHiRes() + 30000;
      // GetModelSize() changes immediately, before the asynchronous Lite
      // network is available. Wait for the actual replacement to finish.
      while (!processor.GetStatusText().endsWith(" (Lite)")) {
        Check(juce::Time::getMillisecondCounterHiRes() < deadline, "Lite model load timed out");
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
      }
    }
    const auto setMode = [&](OutputMode value) {
      mode->setValueNotifyingHost(static_cast<float>(static_cast<int>(value)) / 3.0f);
    };
    setMode(OutputMode::Raw);
    const auto raw = RenderStaticProcessor(processor);
    setMode(OutputMode::Normalized);
    const auto normalized = RenderStaticProcessor(processor);
    CheckOutputScale(raw, normalized, size == ModelSize::Lite ? 6.0 : 4.0,
                     "Static Normalized gain ignored selected child loudness");
    setMode(OutputMode::Calibrated);
    const auto calibrated = RenderStaticProcessor(processor);
    CheckOutputScale(raw, calibrated, -6.0, "Static Calibrated gain is incorrect");
    setMode(OutputMode::Auto);
    CheckOutputScale(normalized, RenderStaticProcessor(processor), 0.0,
                     "Static Auto did not honor child gear_type");

    // With calibration enabled, a different interface level must change the
    // actual signal entering the neural network (not the output trim).
    setMode(OutputMode::Raw);
    calibrateInput->setValueNotifyingHost(1.0f);
    const auto drivenAt12 = RenderStaticProcessor(processor);
    inputCalibration->setValueNotifyingHost(inputCalibration->convertTo0to1(24.0f));
    const auto drivenAt24 = RenderStaticProcessor(processor);
    double driveDelta = 0.0;
    for (size_t i = 0; i < drivenAt12.size(); ++i)
      driveDelta = std::max(driveDelta, std::abs(static_cast<double>(
          drivenAt12[i] - drivenAt24[i])));
    Check(driveDelta > 1.0e-5, "Input Calibration does not change static model drive");
    // Disable drive compensation for the output-mode stability checks.
    calibrateInput->setValueNotifyingHost(0.0f);
    setMode(OutputMode::Raw);
    CheckOutputScale(raw, RenderStaticProcessor(processor), 0.0,
                     "Input calibration changed Raw output with input calibration disabled");
    setMode(OutputMode::Calibrated);
    CheckOutputScale(calibrated, RenderStaticProcessor(processor), 0.0,
                     "Switching to Calibrated reused the changed input calibration");
    outputGain->setValueNotifyingHost(outputGain->convertTo0to1(2.0f));
    CheckOutputScale(calibrated, RenderStaticProcessor(processor), 2.0,
                     "Output Gain unexpectedly recalibrated the output reference");
    setMode(OutputMode::Normalized);
    CheckOutputScale(normalized, RenderStaticProcessor(processor), 2.0,
                     "Normalized mode drifted after input calibration change");
    setMode(OutputMode::Auto);
    CheckOutputScale(normalized, RenderStaticProcessor(processor), 2.0,
                     "Auto mode drifted after repeated mode changes");
  }

  // A missing child loudness must fall back to root metadata (and invalid
  // child loudness must not mask a valid root value).
  for (const bool invalid : {false, true}) {
    auto fallback = staticModel;
    auto& child = fallback["config"]["submodels"][0]["model"]["metadata"];
    if (invalid) child["loudness"] = nullptr;
    else child.erase("loudness");
    const auto fallbackPath = directory / (invalid ? "fallback-invalid.nam" : "fallback-missing.nam");
    Write(fallbackPath, fallback);
    NamModelEngine engine;
    std::string error;
    Check(engine.LoadModel(fallbackPath.string(), error, ModelSize::Lite), error);
    Check(std::abs(engine.GetOutputCalibrationGainDb(12.0,
              namparametric::dsp::OutputMode::Normalized) - 2.0) < 1e-6,
          "Missing/invalid child loudness did not fall back to outer metadata");
    Check(std::abs(engine.GetInputCalibrationGainDb(24.0, true) - 6.0) < 1e-6,
          "Static input calibration did not read outer input_level_dbu");
    Check(std::abs(engine.GetOutputCalibrationGainDb(12.0,
              namparametric::dsp::OutputMode::Calibrated) + 6.0) < 1e-6,
          "Static output calibration did not read outer output_level_dbu");
  }
}

void TestSnapshotAndFiles(const std::filesystem::path& directory) {
  Processor processor;
  Check(!processor.CanExportStaticModel() && !processor.CreateStaticExportSnapshot(),
        "Empty processor offers export");
  const auto path = directory / "Pedal.pnam";
  const auto original = MakeA2(true);
  Write(path, original);
  processor.LoadModelAsync(juce::File(juce::String(path.string())));
  WaitForLoad(processor);
  processor.SetRuntimeParameterValue(0, 6.5);
  processor.SetRuntimeParameterValue(1, 1);
  processor.SetRuntimeParameterValue(2, 2);
  const auto snapshot = processor.CreateStaticExportSnapshot();
  Check(snapshot && snapshot->suggestedFileName == "Pedal (Gain=6.5, Boost=On, Mode=C).nam",
        "Filename does not describe the source and settings");
  Check(Processor::ExportStaticModel(*snapshot, snapshot->sourceFile).failed(),
        "Export overwrote the source model");
  Check(Json::parse(snapshot->sourceFile.loadFileAsString().toStdString()) == original,
        "Source file was changed");
  processor.SetRuntimeParameterValue(0, 9);
  processor.ClearModel();
  Write(path, {{"changed_on_disk", true}});
  Check(!processor.CanExportStaticModel(), "Clear did not disable export");
  const juce::File destination(
      juce::String((directory / snapshot->suggestedFileName.toStdString()).string()));
  const auto result = Processor::ExportStaticModel(*snapshot, destination);
  Check(result.wasOk(), result.getErrorMessage().toStdString());
  const auto saved = Json::parse(destination.loadFileAsString().toStdString());
  Check(saved ==
            NamModelEngine::BakeStaticModel(original, std::vector<float>{6.5f, 1, 2}, "Pedal.pnam"),
        "Export used later controls or re-read the source from disk");
  auto invalid = *snapshot;
  invalid.values.clear();
  Check(Processor::ExportStaticModel(invalid, destination).failed() &&
            Json::parse(destination.loadFileAsString().toStdString()) == saved,
        "Failed export damaged the existing destination");
  Check(
      Processor::ExportStaticModel(*snapshot, destination.getChildFile("unwritable.nam")).failed(),
      "Invalid destination was reported as saved");
  auto unsafe = original;
  unsafe["config"]["params"][0]["name"] = "Gain/Drive";
  for (auto& child : unsafe["config"]["submodels"])
    child["config"]["params"] = unsafe["config"]["params"];
  Write(path, unsafe);
  processor.LoadModelAsync(juce::File(juce::String(path.string())));
  WaitForLoad(processor);
  Check(!processor.CreateStaticExportSnapshot()->suggestedFileName.containsChar('/'),
        "Suggested filename contains a path separator");
  std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
  ModelBarComponent* bar = nullptr;
  for (auto* child : editor->getChildren())
    if (auto* candidate = dynamic_cast<ModelBarComponent*>(child)) bar = candidate;
  Check(bar != nullptr && bar->getExportButton().isEnabled(),
        "Export button is missing or disabled");
  for (const int width : {520, 640}) {
    editor->setSize(width, editor->getHeight());
    const auto button = bar->getExportButton().getBounds();
    Check(bar->getLocalBounds().contains(button) &&
              !button.intersects(bar->getSizeSwitch().getBounds()) &&
              !button.intersects(bar->getClearButton().getBounds()) &&
              !button.intersects(bar->getSelectButton().getBounds()),
          "Export button overlaps other controls");
    juce::MemoryOutputStream image;
    juce::PNGImageFormat png;
    Check(png.writeImageToStream(editor->createComponentSnapshot(editor->getLocalBounds()), image),
          "Could not render export UI");
    Check(
        juce::File(juce::String(
                       (directory / ("export-preview-" + std::to_string(width) + ".png")).string()))
            .replaceWithData(image.getData(), image.getDataSize()),
        "Could not save export UI");
  }
  std::cout << "Snapshot, naming, metadata, file safety and UI tests passed\n";
}
}  // namespace

static void TestChannelRouting() {
  Processor processor;
  auto layout = processor.getBusesLayout();
  layout.inputBuses.set(0, juce::AudioChannelSet::stereo());
  Check(processor.setBusesLayout(layout), "Stereo input layout rejected");
  processor.prepareToPlay(48000.0, 17);
  auto* stereo = processor.mValueTree.getParameter(Processor::ParamIDs::stereo);
  juce::AudioBuffer<float> buffer(2, 40);
  juce::MidiBuffer midi;
  const auto render = [&](bool enabled, float right) {
    stereo->setValueNotifyingHost(enabled ? 1.0f : 0.0f);
    for (int i = 0; i < buffer.getNumSamples(); ++i) {
      buffer.setSample(0, i, 0.25f);
      buffer.setSample(1, i, -0.5f);
    }
    processor.processBlock(buffer, midi);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
      Check(buffer.getSample(0, i) == 0.25f && buffer.getSample(1, i) == right,
            "Invalid mono/stereo channel routing");
  };
  render(false, 0.25f);
  render(true, -0.5f);
  juce::MemoryBlock state;
  processor.getStateInformation(state);
  Processor restored;
  restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
  Check(restored.mValueTree.getRawParameterValue(Processor::ParamIDs::stereo)->load() == 1.0f,
        "Stereo selection was not restored");
}

int main(int argc, const char* const* argv) {
  try {
    juce::ScopedJuceInitialiser_GUI gui;
    TestChannelRouting();
#if JUCE_WINDOWS
    juce::AudioDeviceManager devices;
    bool asioAvailable = false;
    for (const auto* type : devices.getAvailableDeviceTypes())
      asioAvailable = asioAvailable || type->getTypeName() == "ASIO";
    Check(asioAvailable, "Standalone audio backend must offer ASIO on Windows");
#endif
    const auto directory = argc > 1
                               ? std::filesystem::path(argv[1])
                               : std::filesystem::temp_directory_path() / "nam-static-export-test";
    std::filesystem::create_directories(directory);
    TestBaking(directory);
    TestStaticOutputModeStability(directory);
    TestSnapshotAndFiles(directory);
    TestControlTransitions(directory, MakeHyper(3, false), MakeA2(false));
    std::cout << "All static NAM export tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
