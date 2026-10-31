#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "NamParametricPluginProcessor.h"
#include "UI/ModelParametersPanel.h"
#include "json.hpp"

namespace {
using namparametric::dsp::ModelSize;

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename T>
std::vector<T*> FindComponents(juce::Component& root) {
  std::vector<T*> result;
  if (auto* match = dynamic_cast<T*>(&root)) result.push_back(match);
  for (auto* child : root.getChildren()) {
    auto children = FindComponents<T>(*child);
    result.insert(result.end(), children.begin(), children.end());
  }
  return result;
}

void WaitForLoad(NamParametricPluginAudioProcessor& processor, bool dual, ModelSize size) {
  const auto deadline = juce::Time::getMillisecondCounterHiRes() + 30000.0;
  const juce::String suffix = size == ModelSize::Full ? "(Full)" : "(Lite)";
  while (!processor.HasModelLoaded() || (dual && !processor.GetStatusText().endsWith(suffix))) {
    Check(juce::Time::getMillisecondCounterHiRes() < deadline,
          "Load timed out: " + processor.GetStatusText().toStdString());
    juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
  }
}

void RenderAndCompare(NamParametricPluginAudioProcessor& processor, const std::vector<float>& input,
                      const std::vector<float>& expected) {
  // Consume the GUI updates, then reset audio history with those controls applied.
  juce::MidiBuffer midi;
  juce::AudioBuffer<float> flush(2, 1);
  flush.clear();
  processor.processBlock(flush, midi);
  processor.prepareToPlay(48000.0, 257);
  double maximum = 0;
  for (size_t offset = 0; offset < input.size(); offset += 257) {
    const int count = static_cast<int>(std::min<size_t>(257, input.size() - offset));
    juce::AudioBuffer<float> buffer(2, count);
    buffer.clear();
    std::copy_n(input.data() + offset, count, buffer.getWritePointer(0));
    processor.processBlock(buffer, midi);
    for (int i = 0; i < count; ++i) {
      const auto value = buffer.getSample(0, i);
      Check(std::isfinite(value) && value == buffer.getSample(1, i), "Invalid stereo audio");
      maximum = std::max(maximum, std::abs(static_cast<double>(value) - expected[offset + i]));
    }
  }
  Check(maximum < 2e-5,
        "Switch routing disagrees with trainer; max error=" + std::to_string(maximum));
}

void ClickPosition(SegmentedSwitch& control, int position, int count) {
  const juce::Point<float> point((position + 0.5f) * control.getWidth() / count,
                                 control.getHeight() / 2.0f);
  const auto now = juce::Time::getCurrentTime();
  control.mouseUp(juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), point,
                                   juce::ModifierKeys::leftButtonModifier, 1.0f, 0, 0, 0, 0,
                                   &control, &control, now, point, now, 1, false));
}

void TestWideSwitch() {
  ModelParametersPanel panel;
  panel.setBounds(0, 0, 300, 160);
  NamParametricPluginAudioProcessor::RuntimeParameterInfo info;
  info.name = "Many positions";
  info.minValue = 0;
  info.maxValue = 19;
  for (int i = 0; i < 20; ++i) info.enumNames.emplace_back("Position " + juce::String(i + 1));
  panel.RebuildControls({info}, {0});
  const auto switches = FindComponents<SegmentedSwitch>(panel);
  const auto viewports = FindComponents<juce::Viewport>(panel);
  Check(switches.size() == 1 && viewports.size() == 1, "Wide switch was not built");
  auto& control = *switches[0];
  auto& viewport = *viewports[0];
  Check(control.getX() >= 0, "Wide switch has unreachable positions to the left");
  viewport.setViewPosition(100000, 0);
  Check(viewport.getViewArea().getRight() >= control.getRight(),
        "Wide switch has unreachable positions to the right");
  int selected = -1;
  panel.onValueChanged = [&](size_t index, double value) {
    Check(index == 0, "Wide switch changed another control");
    selected = static_cast<int>(value);
  };
  ClickPosition(control, 19, 20);
  Check(selected == 19, "Could not select the last switch position");
}
}  // namespace

void TestSwitchCombinations(const std::filesystem::path& directory) {
  std::ifstream file(directory / "switch_combinations.json");
  const auto fixture = nlohmann::json::parse(file);
  const auto input = fixture.at("input").get<std::vector<float>>();
  const auto& settings = fixture.at("settings");
  Check(settings.size() == 30, "Expected every 3 x 2 x 5 switch combination");
  size_t renders = 0;
  for (const auto& testCase : fixture.at("cases")) {
    const bool dual = testCase.at("dual_size");
    NamParametricPluginAudioProcessor processor;
    processor.prepareToPlay(48000.0, 257);
    processor.LoadModelAsync(
        juce::File(juce::String((directory / testCase.at("model").get<std::string>()).string())));
    WaitForLoad(processor, dual, ModelSize::Full);
    const auto params = processor.GetRuntimeParameters();
    Check(params.size() == 5 && params[0].enumNames.size() == 3 &&
              params[2].enumNames.size() == 2 && params[4].enumNames.size() == 5 &&
              !params[1].IsSwitch() && !params[3].IsSwitch(),
          "Mixed knob and switch definitions were lost");
    for (size_t i = 0; i < params.size(); ++i) {
      Check(processor.GetRuntimeParameterValue(i) == params[i].defaultValue,
            "A control did not load its own default");
    }
    std::unique_ptr<juce::AudioProcessorEditor> editor(processor.createEditor());
    const auto panels = FindComponents<ModelParametersPanel>(*editor);
    Check(panels.size() == 1, "Missing model controls panel");
    auto& panel = *panels[0];
    auto switches = FindComponents<SegmentedSwitch>(panel);
    auto knobs = FindComponents<juce::Slider>(panel);
    Check(switches.size() == 3 && knobs.size() == 2, "Missing independent switch/knob controls");
    for (const auto size : {ModelSize::Full, ModelSize::Lite}) {
      if (!dual && size == ModelSize::Lite) continue;
      if (size == ModelSize::Lite) {
        processor.SetModelSize(size);
        WaitForLoad(processor, true, size);
        const auto previous = settings.back().get<std::vector<double>>();
        for (size_t i = 0; i < previous.size(); ++i) {
          Check(processor.GetRuntimeParameterValue(i) == previous[i],
                "Switch combination was lost when changing A2 size");
        }
      }
      const char* name = size == ModelSize::Full ? "full" : "lite";
      for (size_t row = 0; row < settings.size(); ++row) {
        const auto values = settings[row].get<std::vector<double>>();
        knobs[0]->setValue(values[1], juce::sendNotificationSync);
        knobs[1]->setValue(values[3], juce::sendNotificationSync);
        for (size_t j = 0; j < switches.size(); ++j) {
          const size_t index = 2 * j;
          ClickPosition(*switches[j], static_cast<int>(values[index]),
                        static_cast<int>(params[index].enumNames.size()));
        }
        for (size_t i = 0; i < values.size(); ++i) {
          Check(processor.GetRuntimeParameterValue(i) == values[i],
                "A UI control changed the wrong parameter or another switch");
        }
        RenderAndCompare(processor, input,
                         testCase.at("expected").at(name)[row].get<std::vector<float>>());
        ++renders;
      }
      juce::MemoryBlock state;
      processor.getStateInformation(state);
      NamParametricPluginAudioProcessor restored;
      restored.prepareToPlay(48000.0, 257);
      restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
      WaitForLoad(restored, dual, size);
      const auto values = settings.back().get<std::vector<double>>();
      for (size_t i = 0; i < values.size(); ++i) {
        Check(restored.GetRuntimeParameterValue(i) == values[i],
              "A control was lost from the saved session");
      }
      RenderAndCompare(restored, input,
                       testCase.at("expected").at(name).back().get<std::vector<float>>());
      // Audio-thread values must update the correct UI switch without feeding back.
      size_t notifications = 0;
      panel.onValueChanged = [&](size_t, double) { ++notifications; };
      for (size_t j = 0; j < switches.size(); ++j) {
        panel.SetValue(2 * j, 0);
        Check(switches[j]->getSelectedIndex() == 0, "UI state updated the wrong switch");
        panel.SetValue(2 * j, values[2 * j]);
      }
      Check(notifications == 0, "UI refresh fed back into parameter updates");
      panel.onValueChanged = [&](size_t index, double value) {
        processor.SetRuntimeParameterValue(index, value);
      };
    }
  }
  TestWideSwitch();
  std::cout << renders
            << " multi-switch UI/audio combinations match the trainer; state and sizes preserved\n";
}
