#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

class CalibrationSettingsComponent final : public juce::Component {
 public:
  explicit CalibrationSettingsComponent(juce::AudioProcessorValueTreeState& state);
  void paint(juce::Graphics&) override;
  void resized() override;

 private:
  juce::Label mTitle, mInputLabel, mOutputLabel, mHelp, mTransitionLabel, mTransitionHelp;
  juce::ToggleButton mEnabled{"Calibrate input"};
  juce::Slider mInput{juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight};
  juce::ComboBox mOutput;
  juce::ComboBox mTransition;
  std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> mEnabledAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> mInputAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> mOutputAttachment;
  std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> mTransitionAttachment;
};
