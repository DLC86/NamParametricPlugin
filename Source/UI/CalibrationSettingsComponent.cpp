#include "CalibrationSettingsComponent.h"

#include "NamColours.h"
#include "NamParametricPluginProcessor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

CalibrationSettingsComponent::CalibrationSettingsComponent(
    juce::AudioProcessorValueTreeState& state) {
  mTitle.setText("Settings", juce::dontSendNotification);
  mTitle.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
  mInputLabel.setText("Input calibration", juce::dontSendNotification);
  mOutputLabel.setText("Output mode", juce::dontSendNotification);
  mHelp.setText(
      "Input calibration is your interface's analog input level at 0 dBFS. "
      "Changing it adjusts model drive only. Calibrated output is referenced when "
      "loading a model or changing Output mode/level, as in NAM Oversampler. "
      "Parametric models use single-network 100/100 ms linear weight smoothing.",
      juce::dontSendNotification);
  mHelp.setFont(juce::Font(juce::FontOptions(12.0f)));
  mHelp.setColour(juce::Label::textColourId, nam::ui::Colours::textSecondary);
  // Text-only field. The host parameter retains its ID and automation.
  mInput.setEditable(true, true, false);
  mInput.setJustificationType(juce::Justification::centred);
  mInput.setColour(juce::Label::backgroundColourId, nam::ui::Colours::track);
  mInput.setColour(juce::Label::textColourId, nam::ui::Colours::textPrimary);
  mInput.setColour(juce::Label::outlineColourId, nam::ui::Colours::hairline);
  mInput.setTooltip("Interface input calibration in dBu, up to 3 decimals");
  mOutput.addItemList({"Raw", "Normalized", "Calibrated", "Auto"}, 1);
  for (auto* child : std::initializer_list<juce::Component*>{
      &mTitle, &mInputLabel, &mOutputLabel, &mHelp, &mEnabled, &mInput, &mOutput})
    addAndMakeVisible(child);
  using IDs = NamParametricPluginAudioProcessor::ParamIDs;
  mEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
      state, IDs::calibrateInput, mEnabled);
  auto* parameter = state.getParameter(IDs::inputCalibration);
  jassert(parameter != nullptr);
  if (parameter != nullptr) {
    mInputAttachment = std::make_unique<juce::ParameterAttachment>(
        *parameter, [this](float value) { DisplayInputCalibration(value); },
        state.undoManager);
    mInput.onTextChange = [this, parameter]() {
      auto text = mInput.getText().trim().replaceCharacter(',', '.');
      if (text.endsWithIgnoreCase("dBu"))
        text = text.dropLastCharacters(3).trimEnd();
      const auto utf8 = text.toRawUTF8();
      char* end = nullptr;
      const double value = std::strtod(utf8, &end);
      if (end != utf8 && end != nullptr && *end == '\0' && std::isfinite(value))
        mInputAttachment->setValueAsCompleteGesture(
            static_cast<float>(std::clamp(value, -60.0, 60.0)));
      DisplayInputCalibration(parameter->convertFrom0to1(parameter->getValue()));
    };
    mInputAttachment->sendInitialUpdate();
  }
  mOutputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
      state, IDs::outputMode, mOutput);
  setSize(440, 250);
}

void CalibrationSettingsComponent::DisplayInputCalibration(float value) {
  auto digits = juce::String(value, 3);
  if (digits.containsChar('.'))
    digits = digits.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
  mInput.setText(digits + " dBu", juce::dontSendNotification);
}

void CalibrationSettingsComponent::paint(juce::Graphics& g) {
  g.fillAll(nam::ui::Colours::topBarBackground);
}

void CalibrationSettingsComponent::resized() {
  auto bounds = getLocalBounds().reduced(14);
  mTitle.setBounds(bounds.removeFromTop(28));
  mEnabled.setBounds(bounds.removeFromTop(28));
  auto input = bounds.removeFromTop(32);
  mInputLabel.setBounds(input.removeFromLeft(125));
  mInput.setBounds(input.removeFromLeft(116).reduced(0, 3));
  auto output = bounds.removeFromTop(32);
  mOutputLabel.setBounds(output.removeFromLeft(125));
  mOutput.setBounds(output.reduced(0, 3));
  bounds.removeFromTop(8);
  mHelp.setBounds(bounds);
}
