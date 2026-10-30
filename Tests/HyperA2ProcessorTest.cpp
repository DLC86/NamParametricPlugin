#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "NamParametricPluginProcessor.h"
#include "UI/CalibrationSettingsComponent.h"
#include "UI/ModelBarComponent.h"
#include "json.hpp"

namespace {
using namparametric::dsp::ModelSize;

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void WaitForSize(NamParametricPluginAudioProcessor& processor, ModelSize size) {
  const auto deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
  const juce::String suffix = size == ModelSize::Full ? "(Full)" : "(Lite)";
  while (!processor.GetStatusText().endsWith(suffix)) {
    Check(juce::Time::getMillisecondCounterHiRes() < deadline,
          "Timed out: " + processor.GetStatusText().toStdString());
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  }
  Check(processor.SupportsModelSizeSelection(), "Missing size selector");
  Check(processor.GetModelSize() == size, "Wrong model size selection");
}

void RenderAndCompare(NamParametricPluginAudioProcessor& processor, const std::vector<float>& input,
                      const std::vector<float>& expected) {
  double maximum = 0;
  for (size_t offset = 0; offset < input.size(); offset += 257) {
    const int count = static_cast<int>(std::min<size_t>(257, input.size() - offset));
    juce::AudioBuffer<float> buffer(2, count);
    buffer.clear();
    std::copy_n(input.data() + offset, count, buffer.getWritePointer(0));
    juce::MidiBuffer midi;
    processor.processBlock(buffer, midi);
    for (int i = 0; i < count; ++i) {
      const float value = buffer.getSample(0, i);
      Check(std::isfinite(value) && value == buffer.getSample(1, i), "Invalid stereo audio");
      maximum = std::max(maximum, std::abs(static_cast<double>(value) - expected[offset + i]));
    }
  }
  Check(maximum < 2e-5, "Processor audio disagrees with trainer: " + std::to_string(maximum));
}
}  // namespace

int main(int argc, const char* const* argv) {
  try {
    Check(argc == 2, "Usage: hyper_a2_processor_test <fixture-directory>");
    juce::ScopedJuceInitialiser_GUI gui;
    const std::filesystem::path directory(argv[1]);
    std::ifstream file(directory / "manifest.json");
    const auto manifest = nlohmann::json::parse(file);
    const auto input = manifest.at("input").get<std::vector<float>>();
    const auto& expected = manifest.at("cases")[0].at("expected");
    const juce::File modelFile(juce::String((directory / "hyper_a2_low_rank.nam").string()));

    NamParametricPluginAudioProcessor processor;
    Check(processor.GetModelSize() == ModelSize::Full, "New instances must default to Full");
    ModelBarComponent bar;
    bar.SetSizeSelection(true, true);
    Check(bar.getSizeSwitch().getSelectedIndex() == 0, "UI must default to Full");
    processor.prepareToPlay(48000.0, 257);
    processor.LoadModelAsync(modelFile);
    WaitForSize(processor, ModelSize::Full);
    RenderAndCompare(processor, input, expected.at("full")[0].get<std::vector<float>>());

    processor.SetRuntimeParameterValue(0, 3.0);
    processor.SetModelSize(ModelSize::Lite);
    // A knob moved while the replacement was loading must not revert on completion.
    processor.SetRuntimeParameterValue(0, 8.0);
    processor.SetRuntimeParameterValue(1, 1.0);
    WaitForSize(processor, ModelSize::Lite);
    Check(processor.GetRuntimeParameterValue(0) == 8.0, "Gain was lost during size change");
    Check(processor.GetRuntimeParameterValue(1) == 1.0, "Switch was lost during size change");
    RenderAndCompare(processor, input, expected.at("lite")[1].get<std::vector<float>>());

    juce::MemoryBlock state;
    processor.getStateInformation(state);
    NamParametricPluginAudioProcessor restored;
    restored.prepareToPlay(48000.0, 257);
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    WaitForSize(restored, ModelSize::Lite);
    RenderAndCompare(restored, input, expected.at("lite")[1].get<std::vector<float>>());

    auto xml =
        juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    xml->removeAttribute("hyperA2Size");
    juce::MemoryBlock legacyState;
    juce::AudioProcessor::copyXmlToBinary(*xml, legacyState);
    NamParametricPluginAudioProcessor legacy;
    legacy.prepareToPlay(48000.0, 257);
    legacy.setStateInformation(legacyState.getData(), static_cast<int>(legacyState.getSize()));
    WaitForSize(legacy, ModelSize::Full);
    RenderAndCompare(legacy, input, expected.at("full")[1].get<std::vector<float>>());

    processor.SetModelSize(ModelSize::Full);
    processor.SetModelSize(ModelSize::Lite);
    processor.SetModelSize(ModelSize::Full);
    WaitForSize(processor, ModelSize::Full);
    RenderAndCompare(processor, input, expected.at("full")[1].get<std::vector<float>>());

    auto calibratedFile = directory / "calibrated_processor.nam";
    std::ifstream source(directory / "hyper_a2_low_rank.nam");
    auto calibratedModel = nlohmann::json::parse(source);
    calibratedModel["metadata"]["input_level_dbu"] = 18.0;
    calibratedModel["metadata"]["output_level_dbu"] = 6.0;
    calibratedModel["metadata"]["loudness"] = -24.0;
    calibratedModel["metadata"]["gear_type"] = "studio";
    {
      std::ofstream calibratedStream(calibratedFile);
      calibratedStream << calibratedModel;
    }
    NamParametricPluginAudioProcessor calibrated;
    CalibrationSettingsComponent settings(calibrated.mValueTree);
    Check(calibrated.mValueTree.getRawParameterValue("inputCalibration")->load() == 12.0f,
          "Input calibration default must match standard NAM (+12 dBu)");
    calibrated.prepareToPlay(48000.0, 257);
    calibrated.LoadModelAsync(juce::File(juce::String(calibratedFile.string())));
    WaitForSize(calibrated, ModelSize::Full);
    RenderAndCompare(
        calibrated, input,
        manifest.at("cases")[0].at("calibrated_expected").at("full").get<std::vector<float>>());
    calibrated.mValueTree.getParameter("calibrateInput")->setValueNotifyingHost(0.0f);
    calibrated.mValueTree.getParameter("outputMode")->setValueNotifyingHost(0.0f);
    calibrated.prepareToPlay(48000.0, 257);
    RenderAndCompare(calibrated, input, expected.at("full")[0].get<std::vector<float>>());
    calibrated.mValueTree.getParameter("outputMode")->setValueNotifyingHost(1.0f / 3.0f);
    calibrated.prepareToPlay(48000.0, 257);
    auto normalized = expected.at("full")[0].get<std::vector<float>>();
    for (auto& value : normalized) value *= static_cast<float>(std::pow(10.0, 6.0 / 20));
    RenderAndCompare(calibrated, input, normalized);
    calibrated.getStateInformation(state);
    NamParametricPluginAudioProcessor calibratedRestored;
    calibratedRestored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    Check(calibratedRestored.mValueTree.getRawParameterValue("calibrateInput")->load() == 0.0f &&
              calibratedRestored.mValueTree.getRawParameterValue("outputMode")->load() == 1.0f,
          "Calibration settings were lost from the session");
    // Render the actual components for layout inspection without opening windows.
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto savePreview = [&](juce::Component& component, const char* name) {
      juce::MemoryOutputStream imageData;
      juce::PNGImageFormat png;
      Check(png.writeImageToStream(component.createComponentSnapshot(component.getLocalBounds()),
                                   imageData),
            "Could not render UI preview");
      Check(juce::File(juce::String((directory / name).string()))
                .replaceWithData(imageData.getData(), imageData.getDataSize()),
            "Could not save UI preview");
    };
    savePreview(*editor, "plugin-preview.png");
    settings.setLookAndFeel(&editor->getLookAndFeel());
    savePreview(settings, "calibration-preview.png");
    settings.setLookAndFeel(nullptr);
    std::cout << "HyperA2 processor, Full default, UI, state and switching tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
