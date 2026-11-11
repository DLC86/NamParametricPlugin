#include "CalibrationSettingsComponent.h"

#include "NamColours.h"
#include "NamParametricPluginProcessor.h"

CalibrationSettingsComponent::CalibrationSettingsComponent(
    juce::AudioProcessorValueTreeState& state) {
  mTitle.setText("Settings", juce::dontSendNotification);
  mTitle.setFont(juce::Font(juce::FontOptions(18.0f, juce::Font::bold)));
  mInputLabel.setText("Input calibration", juce::dontSendNotification);
  mOutputLabel.setText("Output mode", juce::dontSendNotification);
  mTransitionLabel.setText("Control smoothing", juce::dontSendNotification);
  mTransitionHelp.setText(
      "Eco: single-network per-sample kernel interpolation. Crossfade: two networks, 50 ms. "
      "Discrete switches always use valid positions.",
      juce::dontSendNotification);
  mTransitionHelp.setFont(juce::Font(juce::FontOptions(12.0f)));
  mTransitionHelp.setColour(juce::Label::textColourId, nam::ui::Colours::textSecondary);
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
  mTransition.addItemList({"Eco de-click (1 network)", "Crossfade (2 networks)"}, 1);
  mTimingLabel.setText("Eco timing (test)", juce::dontSendNotification);
  mCurveLabel.setText("Weight curve (test)", juce::dontSendNotification);
  mTiming.addItemList({"20 / 40 ms (default)", "20 / 60 ms", "20 / 20 ms",
                       "30 / 30 ms", "40 / 40 ms"}, 1);
  mCurve.addItemList({"Smoothstep (cubic)", "Smootherstep (quintic)",
                      "Linear", "Septic (C3)"}, 1);
  mEcoTweakHelp.setText(
      "Temporary experiment settings; active only in Eco. The first number is the "
      "hypernetwork update interval, the second is the coefficient ramp duration. "
      "Changes take effect after the current ramp ends. Default: 20 / 40 ms + Smoothstep.",
      juce::dontSendNotification);
  mEcoTweakHelp.setFont(juce::Font(juce::FontOptions(12.0f)));
  mEcoTweakHelp.setColour(juce::Label::textColourId, nam::ui::Colours::textSecondary);
  for (auto* child : std::initializer_list<juce::Component*>{
           &mTitle, &mInputLabel, &mOutputLabel, &mHelp, &mEnabled, &mInput, &mOutput,
           &mTransitionLabel, &mTransition, &mTransitionHelp,
           &mTimingLabel, &mTiming, &mCurveLabel, &mCurve, &mEcoTweakHelp}) {
    addAndMakeVisible(child);
  }
  using IDs = NamParametricPluginAudioProcessor::ParamIDs;
  mEnabledAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
      state, IDs::calibrateInput, mEnabled);
  mInputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
      state, IDs::inputCalibration, mInput);
  mOutputAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
      state, IDs::outputMode, mOutput);
  mTransitionAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
      state, IDs::controlTransition, mTransition);
  mTimingAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
      state, IDs::ecoTimingPreset, mTiming);
  mCurveAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(
      state, IDs::ecoInterpolationCurve, mCurve);
  setSize(440, 440);
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
  mHelp.setBounds(bounds.removeFromTop(76));
  auto transition = bounds.removeFromTop(32);
  mTransitionLabel.setBounds(transition.removeFromLeft(125));
  mTransition.setBounds(transition.reduced(0, 3));
  bounds.removeFromTop(6);
  mTransitionHelp.setBounds(bounds.removeFromTop(48));
  auto timing = bounds.removeFromTop(32);
  mTimingLabel.setBounds(timing.removeFromLeft(145));
  mTiming.setBounds(timing.reduced(0, 3));
  auto curve = bounds.removeFromTop(32);
  mCurveLabel.setBounds(curve.removeFromLeft(145));
  mCurve.setBounds(curve.reduced(0, 3));
  bounds.removeFromTop(6);
  mEcoTweakHelp.setBounds(bounds);
}
