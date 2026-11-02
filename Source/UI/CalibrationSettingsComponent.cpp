#include "CalibrationSettingsComponent.h"

#include "NamColours.h"
#include "NamParametricPluginProcessor.h"

CalibrationSettingsComponent::CalibrationSettingsComponent(
    juce::AudioProcessorValueTreeState& state) {
  mTitle.setText("Settings", juce::dontSendNotification);
  mTitle.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
  mInputLabel.setText("Input calibration", juce::dontSendNotification);
  mOutputLabel.setText("Output mode", juce::dontSendNotification);
  mHelp.setText(
      "Input calibration is your interface's analog input level at 0 dBFS. "
      "Changing it adjusts model drive only. Calibrated output is referenced when "
      "loading a model or changing Output mode/level, as in NAM Oversampler.",
      juce::dontSendNotification);
  mHelp.setFont(juce::Font(juce::FontOptions(12.0f)));
  mHelp.setColour(juce::Label::textColourId, nam::ui::Colours::textSecondary);
  mInput.setRange(-60.0, 60.0, 0.1);
  mInput.setTextValueSuffix(" dBu");
  mInput.setTextBoxStyle(juce::Slider::TextBoxRight, false, 85, 24);
  mOutput.addItemList({"Raw", "Normalized", "Calibrated", "Auto"}, 1);
  for (auto* child : std::initializer_list<juce::Component*>{
           &mTitle, &mInputLabel, &mOutputLabel, &mHelp, &mEnabled, &mInput, &mOutput}) {
    addAndMakeVisible(child);
  }
  using IDs = NamParametricPluginAudioProcessor::ParamIDs;
  mEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
      state, IDs::calibrateInput, mEnabled);
  mInputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
      state, IDs::inputCalibration, mInput);
  mOutputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
      state, IDs::outputMode, mOutput);
  setSize(360, 235);
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
  mInput.setBounds(input);
  auto output = bounds.removeFromTop(32);
  mOutputLabel.setBounds(output.removeFromLeft(125));
  mOutput.setBounds(output.reduced(0, 3));
  bounds.removeFromTop(8);
  mHelp.setBounds(bounds);
}
