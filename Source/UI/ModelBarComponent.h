#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "SegmentedSwitch.h"

class ModelBarComponent final : public juce::Component {
 public:
  ModelBarComponent();

  void paint(juce::Graphics&) override;
  void resized() override;

  juce::TextButton& getSelectButton() { return mSelectButton; }
  juce::TextButton& getClearButton() { return mClearButton; }
  juce::TextButton& getExportButton() { return mExportButton; }
  void SetModelInfo(bool isLoaded, const juce::String& text);
  SegmentedSwitch& getSizeSwitch() { return mSizeSwitch; }
  void SetSizeSelection(bool available, bool full);

 private:
  juce::TextButton mSelectButton;
  juce::Label mNameLabel;
  juce::TextButton mClearButton;
  juce::TextButton mExportButton{"Export NAM..."};
  SegmentedSwitch mSizeSwitch;
  bool mIsLoaded = false;
  juce::Rectangle<int> mDotBounds;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ModelBarComponent)
};
