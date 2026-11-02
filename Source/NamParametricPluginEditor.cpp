#include "NamParametricPluginEditor.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "UI/CalibrationSettingsComponent.h"
#include "UI/NamColours.h"
#include "TtsBranding.h"

namespace {
constexpr int kTopBarHeight = 110;
constexpr int kModelBarHeight = 64;
constexpr int kIrBarHeight = 64;
constexpr int kDefaultWidth = 640;
constexpr int kMinWidth = 520;
constexpr int kMaxWidth = 1400;
constexpr int kMaxHeight = 900;
constexpr int kAboutButtonSize = 22;
constexpr int kAboutButtonMargin = 14;
}  // namespace

bool NamParametricPluginAudioProcessorEditor::RuntimeParameterListsEqual(
    const std::vector<RuntimeParameterInfo>& lhs, const std::vector<RuntimeParameterInfo>& rhs) {
  if (lhs.size() != rhs.size()) {
    return false;
  }

  for (size_t i = 0; i < lhs.size(); ++i) {
    const auto& a = lhs[i];
    const auto& b = rhs[i];

    if (a.name != b.name || a.enumNames != b.enumNames ||
        !juce::approximatelyEqual(a.defaultValue, b.defaultValue) ||
        !juce::approximatelyEqual(a.minValue, b.minValue) ||
        !juce::approximatelyEqual(a.maxValue, b.maxValue)) {
      return false;
    }
  }

  return true;
}

double NamParametricPluginAudioProcessorEditor::GetInitialRuntimeValue(
    const size_t index, const RuntimeParameterInfo& param) const {
  const auto storedValue = mProcessor.GetRuntimeParameterValue(index).value_or(param.defaultValue);

  if (param.IsSwitch()) {
    return juce::jlimit(0.0, static_cast<double>(param.enumNames.size() - 1),
                        std::round(storedValue));
  }

  double minValue = param.minValue;
  double maxValue = param.maxValue;
  if (maxValue < minValue) {
    std::swap(minValue, maxValue);
  }
  if (juce::approximatelyEqual(minValue, maxValue)) {
    maxValue = minValue + 1.0;
  }

  return juce::jlimit(minValue, maxValue, storedValue);
}

void NamParametricPluginAudioProcessorEditor::RebuildRuntimeParameterControls(
    const std::vector<RuntimeParameterInfo>& params) {
  std::vector<double> initialValues;
  initialValues.reserve(params.size());
  for (size_t index = 0; index < params.size(); ++index) {
    initialValues.push_back(GetInitialRuntimeValue(index, params[index]));
  }

  mParametersPanel.RebuildControls(params, initialValues);
  mLastRuntimeParameters = params;
  UpdateResizeLimits();
}

void NamParametricPluginAudioProcessorEditor::UpdateRuntimeParameterControls() {
  const auto params = mProcessor.GetRuntimeParameters();
  if (!RuntimeParameterListsEqual(mLastRuntimeParameters, params)) {
    RebuildRuntimeParameterControls(params);
  }
  UpdateRuntimeParameterValues();
}

void NamParametricPluginAudioProcessorEditor::UpdateRuntimeParameterValues() {
  for (size_t index = 0; index < mParametersPanel.GetControlCount(); ++index) {
    const auto value = mProcessor.GetRuntimeParameterValue(index);
    if (value.has_value()) {
      mParametersPanel.SetValue(index, *value);
    }
  }
}

void NamParametricPluginAudioProcessorEditor::UpdateModelBarInfo() {
  const bool loaded = mProcessor.HasModelLoaded();
  const auto text =
      loaded ? juce::File(mProcessor.GetModelPath()).getFileName() : mProcessor.GetStatusText();
  mModelBar.SetModelInfo(loaded, text);
  mModelBar.SetSizeSelection(mProcessor.SupportsModelSizeSelection(),
                             mProcessor.GetModelSize() == namparametric::dsp::ModelSize::Full);
  mModelBar.getExportButton().setEnabled(mProcessor.CanExportStaticModel() &&
                                         mExportChooser == nullptr && !mExportFuture);
  mModelBar.getExportButton().setButtonText(mExportFuture ? "Exporting..." : "Export NAM...");
}

void NamParametricPluginAudioProcessorEditor::UpdateIrBarInfo() {
  const bool loaded = mProcessor.HasIrLoaded();
  const auto text =
      loaded ? juce::File(mProcessor.GetIrPath()).getFileName() : mProcessor.GetIrStatusText();
  mIrBar.SetIrInfo(loaded, text);

  const bool enabled = mIrEnabledParameter != nullptr && mIrEnabledParameter->getValue() >= 0.5f;
  mIrBar.getEnabledSwitch().setSelectedIndex(enabled ? 1 : 0, juce::dontSendNotification);
}

void NamParametricPluginAudioProcessorEditor::UpdateResizeLimits() {
  const int minHeight =
      kTopBarHeight + mParametersPanel.GetMinimumContentHeight() + kModelBarHeight + kIrBarHeight;
  setResizeLimits(kMinWidth, minHeight, kMaxWidth, kMaxHeight);
}

void NamParametricPluginAudioProcessorEditor::ShowModelChooser() {
  const juce::File currentModelFile(mProcessor.GetModelPath());
  const juce::File startDirectory =
      currentModelFile.existsAsFile() ? currentModelFile.getParentDirectory() : juce::File();
  mModelChooser =
      std::make_unique<juce::FileChooser>("Select a NAM or parametric model", startDirectory,
                                         "*.nam;*.pnam");

  const int chooserFlags =
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

  mModelChooser->launchAsync(chooserFlags, [this](const juce::FileChooser& chooser) {
    const juce::File selectedFile = chooser.getResult();
    if (selectedFile.existsAsFile()) {
      mProcessor.LoadModelAsync(selectedFile);
    }
    mModelChooser.reset();
  });
}

void NamParametricPluginAudioProcessorEditor::ShowIrChooser() {
  const juce::File currentIrFile(mProcessor.GetIrPath());
  const juce::File startDirectory =
      currentIrFile.existsAsFile() ? currentIrFile.getParentDirectory() : juce::File();
  mIrChooser =
      std::make_unique<juce::FileChooser>("Select an impulse response", startDirectory, "*.wav");

  const int chooserFlags =
      juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

  mIrChooser->launchAsync(chooserFlags, [this](const juce::FileChooser& chooser) {
    const juce::File selectedFile = chooser.getResult();
    if (selectedFile.existsAsFile()) {
      mProcessor.LoadIrAsync(selectedFile);
    }
    mIrChooser.reset();
  });
}

void NamParametricPluginAudioProcessorEditor::ShowStaticExportChooser() {
  if (mExportChooser != nullptr || mExportFuture) return;
  auto snapshot = mProcessor.CreateStaticExportSnapshot();
  if (!snapshot) {
    juce::AlertWindow::showMessageBoxAsync(
        juce::MessageBoxIconType::WarningIcon, "Static NAM export",
        "The model is changing. Please try again once it has loaded.");
    return;
  }
  const auto suggested = snapshot->sourceFile.getSiblingFile(snapshot->suggestedFileName);
  mExportChooser = std::make_unique<juce::FileChooser>("Export static NAM", suggested, "*.nam");
  UpdateModelBarInfo();
  const int flags = juce::FileBrowserComponent::saveMode |
                    juce::FileBrowserComponent::canSelectFiles |
                    juce::FileBrowserComponent::warnAboutOverwriting;
  juce::Component::SafePointer<NamParametricPluginAudioProcessorEditor> safeThis(this);
  mExportChooser->launchAsync(
      flags, [safeThis, snapshot = std::move(*snapshot)](const juce::FileChooser& chooser) {
        if (safeThis == nullptr) return;
        const auto selected = chooser.getResult();
        if (selected != juce::File()) {
          // JUCE's chooser adds the .nam extension. Retain the user's destination exactly,
          // so the overwrite confirmation always refers to the file that will be written.
          safeThis->mExportDestination = selected;
          safeThis->mExportFuture.emplace(std::async(std::launch::async, [snapshot, selected]() {
            return NamParametricPluginAudioProcessor::ExportStaticModel(snapshot, selected);
          }));
        }
        safeThis->mExportChooser.reset();
        safeThis->UpdateModelBarInfo();
      });
}

void NamParametricPluginAudioProcessorEditor::CollectStaticExport() {
  if (!mExportFuture ||
      mExportFuture->wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  const auto result = mExportFuture->get();
  mExportFuture.reset();
  juce::AlertWindow::showMessageBoxAsync(
      result.wasOk() ? juce::MessageBoxIconType::InfoIcon : juce::MessageBoxIconType::WarningIcon,
      "Static NAM export",
      result.wasOk() ? "Saved " + mExportDestination.getFileName() : result.getErrorMessage());
}

NamParametricPluginAudioProcessorEditor::NamParametricPluginAudioProcessorEditor(
    NamParametricPluginAudioProcessor& pluginProcessor)
    : juce::AudioProcessorEditor(&pluginProcessor), mProcessor(pluginProcessor) {
  setLookAndFeel(&mLookAndFeel);

  addAndMakeVisible(mTopBar);
  addAndMakeVisible(mParametersPanel);
  addAndMakeVisible(mModelBar);
  addAndMakeVisible(mIrBar);

  mAboutButton.setColour(juce::TextButton::buttonColourId, nam::ui::Colours::track);
  mAboutButton.setColour(juce::TextButton::textColourOffId, nam::ui::Colours::textSecondary);
  mAboutButton.onClick = [this]() { mAboutOverlay.Show(); };
  addAndMakeVisible(mAboutButton);
  addAndMakeVisible(mSettingsButton);
  mSettingsButton.onClick = [this]() {
    juce::CallOutBox::launchAsynchronously(
        std::make_unique<CalibrationSettingsComponent>(mProcessor.mValueTree),
        mSettingsButton.getBounds(), this);
  };
  addChildComponent(mAboutOverlay);

  mModelBar.getSelectButton().onClick = [this]() { ShowModelChooser(); };
  mModelBar.getClearButton().onClick = [this]() { mProcessor.ClearModel(); };
  mModelBar.getExportButton().onClick = [this]() { ShowStaticExportChooser(); };
  mModelBar.getSizeSwitch().onChange = [this](int index) {
    mProcessor.SetModelSize(index == 0 ? namparametric::dsp::ModelSize::Full
                                       : namparametric::dsp::ModelSize::Lite);
  };

  mIrBar.getSelectButton().onClick = [this]() { ShowIrChooser(); };
  mIrBar.getClearButton().onClick = [this]() { mProcessor.ClearIr(); };

  mIrEnabledParameter =
      mProcessor.mValueTree.getParameter(NamParametricPluginAudioProcessor::ParamIDs::irEnabled);
  mIrBar.getEnabledSwitch().onChange = [this](int index) {
    if (mIrEnabledParameter != nullptr) {
      mIrEnabledParameter->setValueNotifyingHost(index == 1 ? 1.0f : 0.0f);
    }
  };

  mParametersPanel.onValueChanged = [this](size_t index, double value) {
    mProcessor.SetRuntimeParameterValue(index, value);
  };

  mInputAttachment = std::make_unique<SliderAttachment>(
      mProcessor.mValueTree, NamParametricPluginAudioProcessor::ParamIDs::inputGainDb,
      mTopBar.getInputSlider());
  mOutputAttachment = std::make_unique<SliderAttachment>(
      mProcessor.mValueTree, NamParametricPluginAudioProcessor::ParamIDs::outputGainDb,
      mTopBar.getOutputSlider());

  UpdateRuntimeParameterControls();
  UpdateModelBarInfo();
  UpdateIrBarInfo();
  startTimerHz(12);

  setResizable(true, true);
  UpdateResizeLimits();
  const int initialHeight = kTopBarHeight + mParametersPanel.GetMinimumContentHeight() +
                            kModelBarHeight + kIrBarHeight + 30;
  setSize(kDefaultWidth, initialHeight);
}

NamParametricPluginAudioProcessorEditor::~NamParametricPluginAudioProcessorEditor() {
  setLookAndFeel(nullptr);
}

void NamParametricPluginAudioProcessorEditor::paint(juce::Graphics& g) {
  g.fillAll(nam::ui::Colours::background);
}

void NamParametricPluginAudioProcessorEditor::parentHierarchyChanged() {
  if (juce::JUCEApplicationBase::isStandaloneApp())
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
      window->setIcon(juce::ImageCache::getFromMemory(TtsBranding::TTSLogo_png,
                                                    TtsBranding::TTSLogo_pngSize));
}

void NamParametricPluginAudioProcessorEditor::resized() {
  auto bounds = getLocalBounds();
  mTopBar.setBounds(bounds.removeFromTop(kTopBarHeight));
  mIrBar.setBounds(bounds.removeFromBottom(kIrBarHeight));
  mModelBar.setBounds(bounds.removeFromBottom(kModelBarHeight));
  mParametersPanel.setBounds(bounds);

  mAboutButton.setBounds(getWidth() - kAboutButtonSize - kAboutButtonMargin,
                         getHeight() - kAboutButtonSize - kAboutButtonMargin, kAboutButtonSize,
                         kAboutButtonSize);
  mAboutOverlay.setBounds(getLocalBounds());
  mSettingsButton.setBounds(mAboutButton.getX() - 72, mAboutButton.getY(), 64, kAboutButtonSize);
}

void NamParametricPluginAudioProcessorEditor::timerCallback() {
  CollectStaticExport();
  UpdateModelBarInfo();
  UpdateIrBarInfo();
  UpdateRuntimeParameterControls();
}
