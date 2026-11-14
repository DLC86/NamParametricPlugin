#include "TopBarComponent.h"

#include "NamColours.h"
#include <JuceHeader.h> // ProjectInfo::versionString

namespace {
constexpr int kKnobColumnWidth = 108;
constexpr int kKnobSize = 56;
constexpr int kTagHeight = 18;
constexpr int kValueHeight = 18;
constexpr int kColumnGap = 3;
constexpr int kColumnVerticalPadding = 6;
}  // namespace

TopBarComponent::StereoButton::StereoButton() : juce::Button("Stereo") {
  setClickingTogglesState(true);
  setTooltip("Mono / Stereo");
}

void TopBarComponent::StereoButton::paintButton(juce::Graphics& g, bool highlighted,
                                                bool down) {
  auto bounds = getLocalBounds().toFloat().reduced(1.0f);
  g.setColour(highlighted || down ? nam::ui::Colours::hairline : nam::ui::Colours::track);
  g.fillRoundedRectangle(bounds, 5.0f);
  g.setColour(getToggleState() ? nam::ui::Colours::accent : nam::ui::Colours::textSecondary);
  auto circle = bounds.withSizeKeepingCentre(10.0f, 10.0f);
  if (getToggleState()) {
    g.drawEllipse(circle.translated(-3.5f, 0.0f), 1.5f);
    g.drawEllipse(circle.translated(3.5f, 0.0f), 1.5f);
  } else {
    g.drawEllipse(circle, 1.5f);
  }
}

void TopBarComponent::ConfigureTag(juce::Label& label, const juce::String& text) {
  label.setText(text, juce::dontSendNotification);
  label.setJustificationType(juce::Justification::centred);
  label.setFont(juce::Font(juce::FontOptions(14.0f, juce::Font::bold)));
  label.setColour(juce::Label::textColourId, nam::ui::Colours::textSecondary);
}

void TopBarComponent::ConfigureValueLabel(juce::Label& label) {
  label.setJustificationType(juce::Justification::centred);
  label.setFont(juce::Font(juce::FontOptions(14.5f)));
  label.setColour(juce::Label::textColourId, nam::ui::Colours::textTertiary);
}

void TopBarComponent::ConfigureKnob(juce::Slider& knob) {
  knob.setSliderStyle(juce::Slider::RotaryVerticalDrag);
  knob.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
}

TopBarComponent::TopBarComponent() {
  ConfigureTag(mInput.tag, "Input");
  ConfigureTag(mOutput.tag, "Output");
  ConfigureValueLabel(mInput.value);
  ConfigureValueLabel(mOutput.value);
  ConfigureKnob(mInput.knob);
  ConfigureKnob(mOutput.knob);

  addAndMakeVisible(mInput.tag);
  addAndMakeVisible(mInput.knob);
  addAndMakeVisible(mInput.value);
  addAndMakeVisible(mOutput.tag);
  addAndMakeVisible(mOutput.knob);
  addAndMakeVisible(mOutput.value);
  addAndMakeVisible(mStereo);

  mInput.knob.onValueChange = [this]() { UpdateValueLabel(mInput); };
  mOutput.knob.onValueChange = [this]() { UpdateValueLabel(mOutput); };

  UpdateValueLabel(mInput);
  UpdateValueLabel(mOutput);
}

void TopBarComponent::UpdateValueLabel(KnobColumn& knobColumn) {
  knobColumn.value.setText(juce::String(knobColumn.knob.getValue(), 1) + " dB",
                           juce::dontSendNotification);
}

void TopBarComponent::paint(juce::Graphics& g) {
  g.fillAll(nam::ui::Colours::topBarBackground);

  g.setColour(nam::ui::Colours::hairline);
  g.fillRect(getLocalBounds().removeFromBottom(1));

  auto area = mTitleBounds.toNearestInt();
  auto title = area.removeFromTop(74);
  g.setColour(nam::ui::Colours::accent);
  g.setFont(juce::Font(juce::FontOptions(32.0f, juce::Font::bold)));
  g.drawFittedText("PNAM Player", title.withTrimmedTop(12),
                   juce::Justification::centred, 1);
  g.setColour(nam::ui::Colours::textSecondary);
  g.setFont(juce::Font(juce::FontOptions(14.0f)));
  g.drawText("v" + juce::String(ProjectInfo::versionString),
             area.removeFromTop(22), juce::Justification::centred);
}

void TopBarComponent::LayoutColumn(juce::Rectangle<int> column, KnobColumn& knobColumn) {
  auto bounds = column.reduced(6, kColumnVerticalPadding);
  knobColumn.tag.setBounds(bounds.removeFromTop(kTagHeight));
  bounds.removeFromTop(kColumnGap);
  knobColumn.knob.setBounds(
      bounds.removeFromTop(kKnobSize).withSizeKeepingCentre(kKnobSize, kKnobSize));
  bounds.removeFromTop(kColumnGap);
  knobColumn.value.setBounds(bounds.removeFromTop(kValueHeight));
}

void TopBarComponent::resized() {
  auto bounds = getLocalBounds();
  LayoutColumn(bounds.removeFromLeft(kKnobColumnWidth), mInput);
  LayoutColumn(bounds.removeFromRight(kKnobColumnWidth), mOutput);
  const auto output = mOutputKnob.getBounds();
  mStereo.setBounds(output.getX() - 30, output.getCentreY() - 12, 26, 24);
  mTitleBounds = bounds.toFloat();
}
