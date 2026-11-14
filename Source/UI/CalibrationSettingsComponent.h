#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class CalibrationSettingsComponent final : public juce::Component {
 public:
  explicit CalibrationSettingsComponent(juce::AudioProcessorValueTreeState& state);
  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  void DisplayInputCalibration(float value);
  juce::Label mTitle, mInputLabel, mOutputLabel, mHelp;
  juce::ToggleButton mEnabled{"Calibrate input"};
  juce::Label mInput;
  juce::ComboBox mOutput;
  std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> mEnabledAttachment;
  std::unique_ptr<juce::ParameterAttachment> mInputAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> mOutputAttachment;
};
