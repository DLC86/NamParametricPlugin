#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <future>
#include <optional>

#include "NamParametricPluginProcessor.h"
#include "UI/AboutOverlayComponent.h"
#include "UI/IrBarComponent.h"
#include "UI/ModelBarComponent.h"
#include "UI/ModelParametersPanel.h"
#include "UI/NamLookAndFeel.h"
#include "UI/TopBarComponent.h"

class NamParametricPluginAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                                      private juce::Timer {
 public:
  explicit NamParametricPluginAudioProcessorEditor(NamParametricPluginAudioProcessor&);
  ~NamParametricPluginAudioProcessorEditor() override;

  void paint(juce::Graphics&) override;
  void resized() override;
  void parentHierarchyChanged() override;

 private:
  using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
  using RuntimeParameterInfo = NamParametricPluginAudioProcessor::RuntimeParameterInfo;

  static bool RuntimeParameterListsEqual(const std::vector<RuntimeParameterInfo>& lhs,
                                         const std::vector<RuntimeParameterInfo>& rhs);
  void UpdateRuntimeParameterControls();
  void UpdateRuntimeParameterValues();
  void RebuildRuntimeParameterControls(const std::vector<RuntimeParameterInfo>& params);
  double GetInitialRuntimeValue(size_t index, const RuntimeParameterInfo& param) const;
  void UpdateModelBarInfo();
  void UpdateIrBarInfo();
  void ShowModelChooser();
  void ShowIrChooser();
  void ShowStaticExportChooser();
  void CollectStaticExport();
  void UpdateResizeLimits();
  void UpdateVersionNotification();
  void timerCallback() override;

  struct VersionNotice {
    juce::String version;
    juce::String releaseUrl;
  };
  static std::optional<VersionNotice> CheckLatestRelease();

  NamParametricPluginAudioProcessor& mProcessor;
  NamLookAndFeel mLookAndFeel;
  TopBarComponent mTopBar;
  ModelParametersPanel mParametersPanel;
  ModelBarComponent mModelBar;
  IrBarComponent mIrBar;
  juce::TextButton mAboutButton{"i"};
  juce::TextButton mSettingsButton{"Settings"};
  // Hidden until a newer published release is found; no startup popup.
  juce::TextButton mUpdateLed{juce::String::fromUTF8("\xe2\x97\x8f")};
  AboutOverlayComponent mAboutOverlay;
  std::optional<std::future<std::optional<VersionNotice>>> mVersionCheck;
  std::optional<VersionNotice> mNewVersion;
  int mUpdateCheckDelayTicks = 18; // Start ~1.5 s after the editor is opened
  int mUpdateBlinkTicks = 0;

  std::unique_ptr<juce::FileChooser> mModelChooser;
  std::unique_ptr<juce::FileChooser> mIrChooser;
  std::unique_ptr<juce::FileChooser> mExportChooser;
  std::optional<std::future<juce::Result>> mExportFuture;
  juce::File mExportDestination;
  juce::RangedAudioParameter* mIrEnabledParameter = nullptr;

  std::unique_ptr<SliderAttachment> mInputAttachment;
  std::unique_ptr<SliderAttachment> mOutputAttachment;

  std::vector<RuntimeParameterInfo> mLastRuntimeParameters;

  JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(NamParametricPluginAudioProcessorEditor)
};
