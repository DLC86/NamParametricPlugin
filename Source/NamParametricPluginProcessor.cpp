#include "NamParametricPluginProcessor.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>

#include "NamParametricPluginEditor.h"

namespace {
constexpr const char* kModelPathStateKey = "modelPath";
constexpr const char* kModelSizeStateKey = "hyperA2Size";
constexpr const char* kIrPathStateKey = "irPath";
constexpr const char* kRuntimeParamsStateType = "RUNTIME_PARAMS";
constexpr const char* kRuntimeParamStateType = "PARAM";
constexpr const char* kRuntimeParamIndexStateKey = "index";
constexpr const char* kRuntimeParamNameStateKey = "name";
constexpr const char* kRuntimeParamValueStateKey = "value";
constexpr int kMinimumModelResetBlockSize = 2048;

juce::String LegalExportName(const juce::String& text) {
  juce::String result;
  const juce::String illegal("<>:\"/\\|?*");
  for (auto character : text)
    result += juce::String::charToString(
        character < 32 || illegal.containsChar(character) ? '_' : character);
  return result;
}

juce::String ControlValueText(float value) {
  // Shortest decimal that round-trips to the actual float used by the model.
  char buffer[64];
  const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value);
  return juce::String::fromUTF8(buffer, static_cast<int>(converted.ptr - buffer));
}

std::vector<NamParametricPluginAudioProcessor::RuntimeParameterInfo> ConvertRuntimeParameters(
    const std::vector<namparametric::dsp::ModelParameterInfo>& params) {
  std::vector<NamParametricPluginAudioProcessor::RuntimeParameterInfo> out;
  out.reserve(params.size());

  for (const auto& param : params) {
    NamParametricPluginAudioProcessor::RuntimeParameterInfo info;
    info.name = param.name;
    info.defaultValue = param.defaultValue;
    info.minValue = param.minValue;
    info.maxValue = param.maxValue;
    info.displayIndex = param.displayIndex;
    info.enumValues = param.enumValues;
    info.enumNames.reserve(param.enumNames.size());
    for (const auto& enumName : param.enumNames) {
      info.enumNames.emplace_back(enumName);
    }
    out.push_back(std::move(info));
  }

  return out;
}

juce::ValueTree CreateRuntimeParameterState(
    const std::vector<NamParametricPluginAudioProcessor::RuntimeParameterInfo>& params,
    const std::vector<float>& values) {
  juce::ValueTree runtimeTree(kRuntimeParamsStateType);
  const size_t count = std::min(params.size(), values.size());
  for (size_t i = 0; i < count; ++i) {
    juce::ValueTree paramTree(kRuntimeParamStateType);
    paramTree.setProperty(kRuntimeParamIndexStateKey, static_cast<int>(i), nullptr);
    paramTree.setProperty(kRuntimeParamNameStateKey, params[i].name, nullptr);
    paramTree.setProperty(kRuntimeParamValueStateKey, values[i], nullptr);
    runtimeTree.addChild(paramTree, -1, nullptr);
  }
  return runtimeTree;
}
}  // namespace

NamParametricPluginAudioProcessor::RuntimeParameterMailbox::RuntimeParameterMailbox(
    const std::span<const float> initialValues)
    : mSize(initialValues.size()),
      mValues(mSize == 0 ? nullptr : std::make_unique<std::atomic<float>[]>(mSize)) {
  for (size_t i = 0; i < mSize; ++i) {
    mValues[i].store(initialValues[i], std::memory_order_relaxed);
  }
}

void NamParametricPluginAudioProcessor::RuntimeParameterMailbox::Publish(const size_t index,
                                                                         const float value) {
  if (index >= mSize) {
    return;
  }

  std::lock_guard<std::mutex> lock(mWriterMutex);
  mVersion.fetch_add(1, std::memory_order_acq_rel);
  mValues[index].store(value, std::memory_order_relaxed);
  mVersion.fetch_add(1, std::memory_order_release);
}

void NamParametricPluginAudioProcessor::RuntimeParameterMailbox::PublishAll(
    const std::span<const float> values) {
  if (values.size() != mSize) {
    return;
  }

  std::lock_guard<std::mutex> lock(mWriterMutex);
  mVersion.fetch_add(1, std::memory_order_acq_rel);
  for (size_t i = 0; i < mSize; ++i) {
    mValues[i].store(values[i], std::memory_order_relaxed);
  }
  mVersion.fetch_add(1, std::memory_order_release);
}

std::optional<float> NamParametricPluginAudioProcessor::RuntimeParameterMailbox::Read(
    const size_t index) const {
  if (index >= mSize) {
    return std::nullopt;
  }
  return mValues[index].load(std::memory_order_acquire);
}

bool NamParametricPluginAudioProcessor::RuntimeParameterMailbox::TryReadStable(
    const std::span<float> destination, uint64_t& version) const {
  if (destination.size() != mSize) {
    return false;
  }

  constexpr int kMaxAttempts = 3;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    const uint64_t versionBefore = mVersion.load(std::memory_order_acquire);
    if ((versionBefore & 1ULL) != 0) {
      continue;
    }

    for (size_t i = 0; i < mSize; ++i) {
      destination[i] = mValues[i].load(std::memory_order_relaxed);
    }

    const uint64_t versionAfter = mVersion.load(std::memory_order_acquire);
    if (versionBefore == versionAfter && (versionAfter & 1ULL) == 0) {
      version = versionAfter;
      return true;
    }
  }

  return false;
}

NamParametricPluginAudioProcessor::NamParametricPluginAudioProcessor()
    : juce::AudioProcessor(BusesProperties()
                               .withInput("Input", juce::AudioChannelSet::mono(), true)
                               .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      mValueTree(*this, nullptr, "PARAMS", CreateParameterLayout()) {
  mInputGainParam = mValueTree.getRawParameterValue(ParamIDs::inputGainDb);
  mOutputGainParam = mValueTree.getRawParameterValue(ParamIDs::outputGainDb);
  mIrEnabledParam = mValueTree.getRawParameterValue(ParamIDs::irEnabled);
  mCalibrateInputParam = mValueTree.getRawParameterValue(ParamIDs::calibrateInput);
  mInputCalibrationParam = mValueTree.getRawParameterValue(ParamIDs::inputCalibration);
  mOutputModeParam = mValueTree.getRawParameterValue(ParamIDs::outputMode);
  startTimerHz(30);
}

NamParametricPluginAudioProcessor::~NamParametricPluginAudioProcessor() { stopTimer(); }

void NamParametricPluginAudioProcessor::prepareToPlay(const double sampleRate,
                                                      const int samplesPerBlock) {
  const int preparedBlockSize = std::max(1, samplesPerBlock);
  mHostProcessingConfig->generation.fetch_add(1, std::memory_order_acq_rel);
  mHostProcessingConfig->sampleRate.store(sampleRate, std::memory_order_relaxed);
  mHostProcessingConfig->blockSize.store(preparedBlockSize, std::memory_order_relaxed);
  mHostProcessingConfig->generation.fetch_add(1, std::memory_order_release);

  mInputScratch.assign(static_cast<size_t>(preparedBlockSize), 0.0f);
  mOutputScratch.assign(static_cast<size_t>(preparedBlockSize), 0.0f);
  mIrOutputScratch.assign(static_cast<size_t>(preparedBlockSize), 0.0f);

  {
    std::lock_guard<std::mutex> lock(mLoadMutex);
    if (mActiveProcessingState != nullptr) {
      mActiveProcessingState->model->Reset(sampleRate, preparedBlockSize);
    }
    if (mStagedProcessingState != nullptr) {
      mStagedProcessingState->model->Reset(sampleRate, preparedBlockSize);
    }
  }

  {
    std::lock_guard<std::mutex> lock(mIrLoadMutex);
    if (mActiveIr != nullptr) {
      mActiveIr->Reset(sampleRate, preparedBlockSize);
    }
    if (mStagedIr != nullptr) {
      mStagedIr->Reset(sampleRate, preparedBlockSize);
    }
  }
}

void NamParametricPluginAudioProcessor::releaseResources() {}

bool NamParametricPluginAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
  return layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono() &&
         layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void NamParametricPluginAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                                     juce::MidiBuffer& midiMessages) {
  juce::ScopedNoDenormals noDenormals;
  juce::ignoreUnused(midiMessages);

  if (buffer.getNumChannels() == 0 || buffer.getNumSamples() <= 0 || mInputScratch.empty()) {
    return;
  }

  const int numSamples = buffer.getNumSamples();

  TryPromoteStagedModel();
  TryPromoteStagedIr();
  ApplyPendingRuntimeParameterChanges();

  float inputGainDb = mInputGainParam != nullptr ? mInputGainParam->load() : 0.0f;
  float outputGainDb = mOutputGainParam != nullptr ? mOutputGainParam->load() : 0.0f;
  if (mActiveProcessingState != nullptr) {
    const double interfaceLevel = mInputCalibrationParam->load();
    inputGainDb += static_cast<float>(mActiveProcessingState->model->GetInputCalibrationGainDb(
        interfaceLevel, mCalibrateInputParam->load() >= 0.5f));
    auto& state = *mActiveProcessingState;
    const int outputMode = static_cast<int>(mOutputModeParam->load());
    // Capture the output reference once per loaded model. Input Calibration
    // changes drive only, including after selecting another Output Mode or
    // adjusting the manual Output Gain. The old code used the current input
    // calibration on every mode/trim change and could introduce level jumps.
    if (!state.outputReferenceInitialized) {
      state.outputReferenceLevel = interfaceLevel;
      state.outputReferenceInitialized = true;
    }
    if (outputMode != state.lastOutputMode) {
      state.outputCalibrationGainDb = state.model->GetOutputCalibrationGainDb(
          state.outputReferenceLevel, static_cast<namparametric::dsp::OutputMode>(outputMode));
      state.lastOutputMode = outputMode;
    }
    outputGainDb += static_cast<float>(state.outputCalibrationGainDb);
  }
  const float inputGain = juce::Decibels::decibelsToGain(inputGainDb);
  const float outputGain = juce::Decibels::decibelsToGain(outputGainDb);
  const bool irEnabled = mIrEnabledParam != nullptr && mIrEnabledParam->load() >= 0.5f;

  const float* input = buffer.getReadPointer(0);
  float* outputLeft = buffer.getWritePointer(0);
  float* outputRight = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : nullptr;
  const int chunkCapacity = static_cast<int>(mInputScratch.size());

  for (int offset = 0; offset < numSamples; offset += chunkCapacity) {
    const int chunkSize = std::min(chunkCapacity, numSamples - offset);
    for (int i = 0; i < chunkSize; ++i) {
      mInputScratch[static_cast<size_t>(i)] = input[offset + i] * inputGain;
    }

    if (mActiveProcessingState != nullptr && mActiveProcessingState->model->IsLoaded()) {
      mActiveProcessingState->model->Process(mInputScratch.data(), mOutputScratch.data(),
                                             chunkSize);
    } else {
      std::copy_n(mInputScratch.data(), chunkSize, mOutputScratch.data());
    }

    if (irEnabled && mActiveIr != nullptr && mActiveIr->IsLoaded()) {
      mActiveIr->Process(mOutputScratch.data(), mIrOutputScratch.data(), chunkSize);
      std::swap(mOutputScratch, mIrOutputScratch);
    }

    for (int i = 0; i < chunkSize; ++i) {
      const float sample = mOutputScratch[static_cast<size_t>(i)] * outputGain;
      outputLeft[offset + i] = sample;
      if (outputRight != nullptr) {
        outputRight[offset + i] = sample;
      }
    }
  }
}

juce::AudioProcessorEditor* NamParametricPluginAudioProcessor::createEditor() {
  return new NamParametricPluginAudioProcessorEditor(*this);
}

juce::AudioProcessorValueTreeState::ParameterLayout
NamParametricPluginAudioProcessor::CreateParameterLayout() {
  std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

  const juce::NormalisableRange<float> gainRange(-12.0f, 12.0f, 0.01f);
  const auto gainAttrs = juce::AudioParameterFloatAttributes().withLabel("dB");

  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID(ParamIDs::inputGainDb, 1), "Input Gain", gainRange, 0.0f, gainAttrs));
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID(ParamIDs::outputGainDb, 1), "Output Gain", gainRange, 0.0f, gainAttrs));
  params.push_back(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID(ParamIDs::irEnabled, 1), "IR Enabled", true));
  params.push_back(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID(ParamIDs::calibrateInput, 1), "Calibrate Input", true));
  params.push_back(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID(ParamIDs::inputCalibration, 1), "Input Calibration",
      juce::NormalisableRange<float>(-60.0f, 60.0f, 0.1f), 12.0f,
      juce::AudioParameterFloatAttributes().withLabel("dBu")));
  params.push_back(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID(ParamIDs::outputMode, 1), "Output Mode",
      juce::StringArray{"Raw", "Normalized", "Calibrated", "Auto"}, 3));
  return {params.begin(), params.end()};
}

void NamParametricPluginAudioProcessor::TryPromoteStagedModel() {
  std::unique_lock<std::mutex> lock(mLoadMutex, std::try_to_lock);
  if (!lock.owns_lock() || mRetiredProcessingState != nullptr) {
    return;
  }

  if (mClearModelStaged) {
    mRetiredProcessingState = std::move(mActiveProcessingState);
    mClearModelStaged = false;
    return;
  }
  if (mStagedProcessingState == nullptr) {
    return;
  }

  mRetiredProcessingState = std::move(mActiveProcessingState);
  mActiveProcessingState = std::move(mStagedProcessingState);
}

void NamParametricPluginAudioProcessor::TryPromoteStagedIr() {
  std::unique_lock<std::mutex> lock(mIrLoadMutex, std::try_to_lock);
  if (!lock.owns_lock() || mRetiredIr != nullptr) {
    return;
  }

  if (mIrClearStaged) {
    mRetiredIr = std::move(mActiveIr);
    mIrClearStaged = false;
    return;
  }
  if (mStagedIr == nullptr) {
    return;
  }

  mRetiredIr = std::move(mActiveIr);
  mActiveIr = std::move(mStagedIr);
}

void NamParametricPluginAudioProcessor::LoadModelAsync(const juce::File& modelFile) {
  CollectCompletedModelLoad();
  StartModelLoad(modelFile);
}

void NamParametricPluginAudioProcessor::ClearModel() { StageModelClear(); }

void NamParametricPluginAudioProcessor::StartModelLoad(
    const juce::File& modelFile, std::vector<RestoredRuntimeParameterValue> restoredValues,
    const bool preserveRuntimeParameters) {
  if (!modelFile.existsAsFile()) {
    std::lock_guard<std::mutex> lock(mLoadMutex);
    mStatusText = "Model file does not exist: " + modelFile.getFullPathName();
    return;
  }

  std::lock_guard<std::mutex> lock(mLoadMutex);
  mRequestedModelPath = modelFile.getFullPathName();
  mRequestedRestoredValues = restoredValues;
  LoadRequest request{modelFile, std::move(restoredValues), ++mLatestLoadRequestId, mModelSize,
                      preserveRuntimeParameters};
  if (mLoadFuture.has_value()) {
    mQueuedLoadRequest = std::move(request);
    mStatusText = "Queued model: " + modelFile.getFileName();
    return;
  }

  BeginModelLoadLocked(std::move(request));
}

void NamParametricPluginAudioProcessor::BeginModelLoadLocked(LoadRequest request) {
  const juce::String fullPath = request.modelFile.getFullPathName();
  const std::string modelPath = fullPath.toStdString();
  const juce::String fileName = request.modelFile.getFileName();
  const auto hostConfig = mHostProcessingConfig;

  mStatusText = "Loading model: " + fileName;
  mLoadFuture.emplace(std::async(std::launch::async, [modelPath, fullPath, hostConfig,
                                                      loadRequest = std::move(request)]() mutable {
    AsyncLoadResult result;
    result.requestId = loadRequest.requestId;
    result.preserveRuntimeParameters = loadRequest.preserveRuntimeParameters;
    result.restoredValues = std::move(loadRequest.restoredValues);
    auto model = std::make_unique<namparametric::dsp::NamModelEngine>();

    std::string error;
    if (!model->LoadModel(modelPath, error, loadRequest.size)) {
      result.message = "Failed to load model: " + juce::String(error);
      return result;
    }

    for (;;) {
      const uint64_t generationBefore = hostConfig->generation.load(std::memory_order_acquire);
      if ((generationBefore & 1ULL) != 0) {
        continue;
      }
      const double sampleRate = hostConfig->sampleRate.load(std::memory_order_relaxed);
      const int blockSize = std::max(hostConfig->blockSize.load(std::memory_order_relaxed),
                                     kMinimumModelResetBlockSize);
      if (generationBefore != hostConfig->generation.load(std::memory_order_acquire)) {
        continue;
      }
      model->Reset(sampleRate, blockSize);
      if (generationBefore == hostConfig->generation.load(std::memory_order_acquire)) {
        break;
      }
    }

    auto processingState = std::make_unique<ProcessingState>();
    processingState->pendingValues = model->GetParameterValues();
    processingState->mailbox =
        std::make_shared<RuntimeParameterMailbox>(processingState->pendingValues);
    processingState->model = std::move(model);

    result.success = true;
    result.loadedPath = fullPath;
    result.runtimeParameters =
        ConvertRuntimeParameters(processingState->model->GetParameterInfos());
    result.message = "Loaded model: " + juce::File(fullPath).getFileName();
    if (processingState->model->SupportsSizeSelection()) {
      result.message +=
          loadRequest.size == namparametric::dsp::ModelSize::Full ? " (Full)" : " (Lite)";
    }
    result.processingState = std::move(processingState);
    return result;
  }));
}

void NamParametricPluginAudioProcessor::CollectCompletedModelLoad() {
  std::lock_guard<std::mutex> lock(mLoadMutex);
  mRetiredProcessingState.reset();

  if (!mLoadFuture.has_value() ||
      mLoadFuture->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    return;
  }

  AsyncLoadResult result = mLoadFuture->get();
  mLoadFuture.reset();
  if (result.requestId == mLatestLoadRequestId) {
    if (!result.success) {
      mStatusText = result.message;
      const auto* previous = mStagedProcessingState != nullptr ? mStagedProcessingState.get()
                                                               : mActiveProcessingState.get();
      mModelSize = previous != nullptr ? previous->model->GetModelSize()
                                       : namparametric::dsp::ModelSize::Full;
      mRequestedModelPath = mModelPath;
    } else {
      if (result.preserveRuntimeParameters && result.loadedPath == mModelPath &&
          mRuntimeParameterMailbox != nullptr) {
        // Read at completion, so knobs turned during a size reload are retained too.
        result.restoredValues.clear();
        for (size_t i = 0; i < mRuntimeParameters.size(); ++i) {
          if (const auto value = mRuntimeParameterMailbox->Read(i)) {
            result.restoredValues.push_back(
                {static_cast<int>(i), mRuntimeParameters[i].name.toStdString(), *value});
          }
        }
      }
      ApplyRestoredValues(result);
      mSupportsModelSizeSelection = result.processingState->model->SupportsSizeSelection();
      mExportSource = result.processingState->model->GetExportSource();
      mStagedProcessingState = std::move(result.processingState);
      mRuntimeParameterMailbox = mStagedProcessingState->mailbox;
      mRuntimeParameters = std::move(result.runtimeParameters);
      mModelPath = result.loadedPath;
      mStatusText = result.message;
      mClearModelStaged = false;
    }
  }

  if (mQueuedLoadRequest.has_value()) {
    auto queuedRequest = std::move(*mQueuedLoadRequest);
    mQueuedLoadRequest.reset();
    BeginModelLoadLocked(std::move(queuedRequest));
  }
}

void NamParametricPluginAudioProcessor::ApplyRestoredValues(AsyncLoadResult& result) {
  if (result.processingState == nullptr || result.processingState->mailbox == nullptr ||
      result.restoredValues.empty()) {
    return;
  }

  auto& values = result.processingState->pendingValues;
  auto& params = result.runtimeParameters;
  std::vector<bool> matched(params.size(), false);
  std::vector<bool> used(result.restoredValues.size(), false);

  const auto apply = [&](const size_t paramIndex, const size_t restoredIndex) {
    const auto& param = params[paramIndex];
    float value = result.restoredValues[restoredIndex].value;
    if (param.IsSwitch()) {
      const float maximum = static_cast<float>(param.enumNames.size() - 1);
      value = std::clamp(std::round(value), 0.0f, maximum);
    } else {
      value =
          std::clamp(value, static_cast<float>(param.minValue), static_cast<float>(param.maxValue));
    }
    values[paramIndex] = value;
    matched[paramIndex] = true;
    used[restoredIndex] = true;
  };

  for (size_t restoredIndex = 0; restoredIndex < result.restoredValues.size(); ++restoredIndex) {
    const auto& restored = result.restoredValues[restoredIndex];
    if (restored.index < 0 || static_cast<size_t>(restored.index) >= params.size()) {
      continue;
    }
    const size_t paramIndex = static_cast<size_t>(restored.index);
    if (params[paramIndex].name.toStdString() == restored.name) {
      apply(paramIndex, restoredIndex);
    }
  }

  for (size_t restoredIndex = 0; restoredIndex < result.restoredValues.size(); ++restoredIndex) {
    if (used[restoredIndex]) {
      continue;
    }
    const auto& restored = result.restoredValues[restoredIndex];
    size_t matchingIndex = params.size();
    int matches = 0;
    for (size_t paramIndex = 0; paramIndex < params.size(); ++paramIndex) {
      if (!matched[paramIndex] && params[paramIndex].name.toStdString() == restored.name) {
        matchingIndex = paramIndex;
        ++matches;
      }
    }
    if (matches == 1) {
      apply(matchingIndex, restoredIndex);
    }
  }

  result.processingState->mailbox->PublishAll(values);
  // Settle the newly constructed network on its restored controls before handing
  // it to the audio thread. Otherwise its initial history still belongs to the
  // defaults, which is especially audible when switching A2 sizes.
  if (result.processingState->model->SetParameterValues(values)) {
    for (;;) {
      const auto generation = mHostProcessingConfig->generation.load(std::memory_order_acquire);
      if ((generation & 1ULL) != 0) continue;
      const auto rate = mHostProcessingConfig->sampleRate.load(std::memory_order_relaxed);
      const auto block = std::max(mHostProcessingConfig->blockSize.load(std::memory_order_relaxed),
                                  kMinimumModelResetBlockSize);
      if (generation != mHostProcessingConfig->generation.load(std::memory_order_acquire)) continue;
      result.processingState->model->Reset(rate, block);
      if (generation == mHostProcessingConfig->generation.load(std::memory_order_acquire)) break;
    }
    result.processingState->mailbox->TryReadStable(values, result.processingState->consumedVersion);
  }
}

void NamParametricPluginAudioProcessor::StageModelClear() {
  std::lock_guard<std::mutex> lock(mLoadMutex);
  mRequestedModelPath.clear();
  mRequestedRestoredValues.clear();
  mSupportsModelSizeSelection = false;
  ++mLatestLoadRequestId;
  mQueuedLoadRequest.reset();
  mStagedProcessingState.reset();
  mRuntimeParameterMailbox.reset();
  mExportSource.reset();
  mRuntimeParameters.clear();
  mModelPath.clear();
  mStatusText = "No model loaded";
  mClearModelStaged = true;
}

void NamParametricPluginAudioProcessor::LoadIrAsync(const juce::File& irFile) {
  CollectCompletedIrLoad();
  StartIrLoad(irFile);

  if (irFile.existsAsFile()) {
    if (auto* enabledParam = mValueTree.getParameter(ParamIDs::irEnabled)) {
      enabledParam->setValueNotifyingHost(1.0f);
    }
  }
}

void NamParametricPluginAudioProcessor::StartIrLoad(const juce::File& irFile) {
  if (!irFile.existsAsFile()) {
    std::lock_guard<std::mutex> lock(mIrLoadMutex);
    mIrStatusText = "IR file does not exist: " + irFile.getFullPathName();
    return;
  }

  std::lock_guard<std::mutex> lock(mIrLoadMutex);
  IrLoadRequest request{irFile, ++mLatestIrLoadRequestId};
  if (mIrLoadFuture.has_value()) {
    mQueuedIrLoadRequest = std::move(request);
    mIrStatusText = "Queued IR: " + irFile.getFileName();
    return;
  }

  BeginIrLoadLocked(std::move(request));
}

void NamParametricPluginAudioProcessor::BeginIrLoadLocked(IrLoadRequest request) {
  const juce::String fullPath = request.irFile.getFullPathName();
  const std::string irPath = fullPath.toStdString();
  const juce::String fileName = request.irFile.getFileName();
  const auto hostConfig = mHostProcessingConfig;

  mIrStatusText = "Loading IR: " + fileName;
  mIrLoadFuture.emplace(std::async(
      std::launch::async,
      [irPath, fullPath, hostConfig, loadRequest = std::move(request)]() mutable {
        IrAsyncLoadResult result;
        result.requestId = loadRequest.requestId;
        auto ir = std::make_unique<namparametric::dsp::IrEngine>();

        std::string error;
        double sampleRate = 48000.0;
        int blockSize = kMinimumModelResetBlockSize;
        for (;;) {
          const uint64_t generationBefore = hostConfig->generation.load(std::memory_order_acquire);
          if ((generationBefore & 1ULL) != 0) {
            continue;
          }
          sampleRate = hostConfig->sampleRate.load(std::memory_order_relaxed);
          blockSize = std::max(hostConfig->blockSize.load(std::memory_order_relaxed),
                               kMinimumModelResetBlockSize);
          if (generationBefore == hostConfig->generation.load(std::memory_order_acquire)) {
            break;
          }
        }

        if (!ir->LoadIr(irPath, sampleRate, error)) {
          result.message = "Failed to load IR: " + juce::String(error);
          return result;
        }
        ir->Reset(sampleRate, blockSize);

        result.success = true;
        result.loadedPath = fullPath;
        result.message = "Loaded IR: " + juce::File(fullPath).getFileName();
        result.irEngine = std::move(ir);
        return result;
      }));
}

void NamParametricPluginAudioProcessor::CollectCompletedIrLoad() {
  std::lock_guard<std::mutex> lock(mIrLoadMutex);
  mRetiredIr.reset();

  if (!mIrLoadFuture.has_value() ||
      mIrLoadFuture->wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
    return;
  }

  IrAsyncLoadResult result = mIrLoadFuture->get();
  mIrLoadFuture.reset();
  if (result.requestId == mLatestIrLoadRequestId) {
    if (!result.success) {
      mIrStatusText = result.message;
    } else {
      mStagedIr = std::move(result.irEngine);
      mIrPath = result.loadedPath;
      mIrStatusText = result.message;
      mIrClearStaged = false;
    }
  }

  if (mQueuedIrLoadRequest.has_value()) {
    auto queuedRequest = std::move(*mQueuedIrLoadRequest);
    mQueuedIrLoadRequest.reset();
    BeginIrLoadLocked(std::move(queuedRequest));
  }
}

void NamParametricPluginAudioProcessor::ClearIr() { StageIrClear(); }

void NamParametricPluginAudioProcessor::StageIrClear() {
  std::lock_guard<std::mutex> lock(mIrLoadMutex);
  ++mLatestIrLoadRequestId;
  mQueuedIrLoadRequest.reset();
  mStagedIr.reset();
  mIrPath.clear();
  mIrStatusText = "No IR loaded";
  mIrClearStaged = true;
}

juce::String NamParametricPluginAudioProcessor::GetIrStatusText() const {
  std::lock_guard<std::mutex> lock(mIrLoadMutex);
  return mIrStatusText;
}

juce::String NamParametricPluginAudioProcessor::GetIrPath() const {
  std::lock_guard<std::mutex> lock(mIrLoadMutex);
  return mIrPath;
}

bool NamParametricPluginAudioProcessor::HasIrLoaded() const {
  std::lock_guard<std::mutex> lock(mIrLoadMutex);
  return !mIrClearStaged && (mActiveIr != nullptr || mStagedIr != nullptr);
}

juce::String NamParametricPluginAudioProcessor::GetStatusText() const {
  std::lock_guard<std::mutex> lock(mLoadMutex);
  return mStatusText;
}

juce::String NamParametricPluginAudioProcessor::GetModelPath() const {
  std::lock_guard<std::mutex> lock(mLoadMutex);
  return mModelPath;
}

bool NamParametricPluginAudioProcessor::HasModelLoaded() const {
  std::lock_guard<std::mutex> lock(mLoadMutex);
  return !mClearModelStaged &&
         (mActiveProcessingState != nullptr || mStagedProcessingState != nullptr);
}

bool NamParametricPluginAudioProcessor::SupportsModelSizeSelection() const {
  std::lock_guard<std::mutex> lock(mLoadMutex);
  return mSupportsModelSizeSelection && !mClearModelStaged;
}

namparametric::dsp::ModelSize NamParametricPluginAudioProcessor::GetModelSize() const {
  std::lock_guard<std::mutex> lock(mLoadMutex);
  return mModelSize;
}

void NamParametricPluginAudioProcessor::SetModelSize(const namparametric::dsp::ModelSize size) {
  juce::String path;
  std::vector<RestoredRuntimeParameterValue> restored;
  bool preserve = false;
  {
    std::lock_guard<std::mutex> lock(mLoadMutex);
    if (size == mModelSize) {
      return;
    }
    mModelSize = size;
    path = mRequestedModelPath.isEmpty() ? mModelPath : mRequestedModelPath;
    restored = mRequestedRestoredValues;
    preserve = path == mModelPath;
  }
  if (path.isNotEmpty()) {
    StartModelLoad(juce::File(path), std::move(restored), preserve);
  }
}

std::vector<NamParametricPluginAudioProcessor::RuntimeParameterInfo>
NamParametricPluginAudioProcessor::GetRuntimeParameters() const {
  std::lock_guard<std::mutex> lock(mLoadMutex);
  return mRuntimeParameters;
}

void NamParametricPluginAudioProcessor::SetRuntimeParameterValue(const size_t index,
                                                                 const double value) {
  std::shared_ptr<RuntimeParameterMailbox> mailbox;
  float sanitizedValue = 0.0f;
  {
    std::lock_guard<std::mutex> lock(mLoadMutex);
    if (index >= mRuntimeParameters.size() || !std::isfinite(value)) {
      return;
    }

    const auto& param = mRuntimeParameters[index];
    if (param.IsSwitch()) {
      sanitizedValue = static_cast<float>(
          std::clamp(std::round(value), 0.0, static_cast<double>(param.enumNames.size() - 1)));
    } else {
      sanitizedValue = static_cast<float>(std::clamp(value, param.minValue, param.maxValue));
    }
    mailbox = mRuntimeParameterMailbox;
  }

  if (mailbox != nullptr) {
    mailbox->Publish(index, sanitizedValue);
  }
}

std::optional<double> NamParametricPluginAudioProcessor::GetRuntimeParameterValue(
    const size_t index) const {
  std::shared_ptr<RuntimeParameterMailbox> mailbox;
  {
    std::lock_guard<std::mutex> lock(mLoadMutex);
    mailbox = mRuntimeParameterMailbox;
  }
  if (mailbox == nullptr) {
    return std::nullopt;
  }
  const auto value = mailbox->Read(index);
  return value.has_value() ? std::optional<double>(*value) : std::nullopt;
}

bool NamParametricPluginAudioProcessor::CanExportStaticModel() const {
  std::lock_guard<std::mutex> lock(mLoadMutex);
  return mExportSource != nullptr && !mClearModelStaged && !mLoadFuture && !mQueuedLoadRequest;
}

std::optional<NamParametricPluginAudioProcessor::StaticExportSnapshot>
NamParametricPluginAudioProcessor::CreateStaticExportSnapshot() const {
  std::lock_guard<std::mutex> lock(mLoadMutex);
  if (mExportSource == nullptr || mClearModelStaged || mLoadFuture || mQueuedLoadRequest ||
      mRuntimeParameterMailbox == nullptr)
    return std::nullopt;

  StaticExportSnapshot snapshot;
  snapshot.source = mExportSource;
  snapshot.sourceFile = juce::File(mModelPath);
  snapshot.values.resize(mRuntimeParameters.size());
  uint64_t version = 0;
  if (!mRuntimeParameterMailbox->TryReadStable(snapshot.values, version)) return std::nullopt;
  juce::StringArray settings;
  for (size_t index = 0; index < mRuntimeParameters.size(); ++index) {
    const auto& parameter = mRuntimeParameters[index];
    const float value = snapshot.values[index];
    const auto label = parameter.IsSwitch() ? parameter.enumNames.at(static_cast<size_t>(value))
                                            : ControlValueText(value);
    settings.add(parameter.name + "=" + label);
  }
  snapshot.suggestedFileName = LegalExportName(snapshot.sourceFile.getFileNameWithoutExtension() +
                                               " (" + settings.joinIntoString(", ") + ").nam");
  return snapshot;
}

juce::Result NamParametricPluginAudioProcessor::ExportStaticModel(
    const StaticExportSnapshot& snapshot, const juce::File& destination) {
  try {
    if (snapshot.source == nullptr) return juce::Result::fail("No parametric model to export.");
    if (destination == snapshot.sourceFile)
      return juce::Result::fail("Choose a different filename to preserve the parametric model.");
    const auto model = namparametric::dsp::NamModelEngine::BakeStaticModel(
        *snapshot.source, snapshot.values, snapshot.sourceFile.getFileName().toStdString());
    const auto text = model.dump(2);
    // A failed write leaves an existing destination intact.
    juce::TemporaryFile temporary(destination);
    {
      juce::FileOutputStream stream(temporary.getFile());
      if (!stream.openedOk()) return stream.getStatus();
      if (!stream.write(text.data(), text.size()))
        return juce::Result::fail("Could not write the NAM file.");
      stream.flush();
      if (stream.getStatus().failed()) return stream.getStatus();
    }
    if (!temporary.overwriteTargetFileWithTemporary())
      return juce::Result::fail("Could not save the NAM file to the selected destination.");
    return juce::Result::ok();
  } catch (const std::exception& error) {
    return juce::Result::fail(juce::String::fromUTF8(error.what()));
  }
}

void NamParametricPluginAudioProcessor::ApplyPendingRuntimeParameterChanges() {
  if (mActiveProcessingState == nullptr || mActiveProcessingState->mailbox == nullptr ||
      mActiveProcessingState->pendingValues.empty()) {
    return;
  }

  uint64_t version = mActiveProcessingState->consumedVersion;
  if (!mActiveProcessingState->mailbox->TryReadStable(mActiveProcessingState->pendingValues,
                                                      version) ||
      version == mActiveProcessingState->consumedVersion) {
    return;
  }

  if (mActiveProcessingState->model->SetParameterValues(mActiveProcessingState->pendingValues)) {
    mActiveProcessingState->consumedVersion = version;
  }
}

void NamParametricPluginAudioProcessor::getStateInformation(juce::MemoryBlock& destData) {
  CollectCompletedModelLoad();
  CollectCompletedIrLoad();
  auto state = mValueTree.copyState();
  juce::String modelPath;
  std::vector<RuntimeParameterInfo> params;
  std::shared_ptr<RuntimeParameterMailbox> mailbox;

  {
    std::lock_guard<std::mutex> lock(mLoadMutex);
    modelPath = mModelPath;
    params = mRuntimeParameters;
    mailbox = mRuntimeParameterMailbox;
  }
  state.setProperty(kModelPathStateKey, modelPath, nullptr);
  state.setProperty(kModelSizeStateKey,
                    GetModelSize() == namparametric::dsp::ModelSize::Lite ? "lite" : "full",
                    nullptr);
  state.setProperty(kIrPathStateKey, GetIrPath(), nullptr);

  auto child = state.getChildWithName(kRuntimeParamsStateType);
  while (child.isValid()) {
    state.removeChild(child, nullptr);
    child = state.getChildWithName(kRuntimeParamsStateType);
  }

  if (mailbox != nullptr && mailbox->Size() == params.size()) {
    std::vector<float> values(params.size());
    uint64_t version = 0;
    if (mailbox->TryReadStable(values, version)) {
      state.addChild(CreateRuntimeParameterState(params, values), -1, nullptr);
    }
  }

  std::unique_ptr<juce::XmlElement> xml(state.createXml());
  copyXmlToBinary(*xml, destData);
}

void NamParametricPluginAudioProcessor::setStateInformation(const void* data,
                                                            const int sizeInBytes) {
  std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));
  if (xml == nullptr) {
    return;
  }

  auto tree = juce::ValueTree::fromXml(*xml);
  if (!tree.isValid() || !tree.hasType(mValueTree.state.getType())) {
    return;
  }

  // Remove obsolete experimental choices from old sessions. All sessions now
  // use the same fixed 100/100 ms linear weight smoothing, regardless of
  // their previous Control smoothing setting.
  for (const char* oldId : {"controlTransition", "ecoTimingPreset", "ecoInterpolationCurve"}) {
    auto oldParameter = tree.getChildWithProperty("id", oldId);
    if (oldParameter.isValid()) tree.removeChild(oldParameter, nullptr);
  }
  mValueTree.replaceState(tree);
  {
    std::lock_guard<std::mutex> lock(mLoadMutex);
    // Old sessions and models without a size preference always start at Full.
    mModelSize = tree.getProperty(kModelSizeStateKey).toString() == "lite"
                     ? namparametric::dsp::ModelSize::Lite
                     : namparametric::dsp::ModelSize::Full;
  }
  std::vector<RestoredRuntimeParameterValue> restoredValues;
  const auto runtimeTree = tree.getChildWithName(kRuntimeParamsStateType);
  if (runtimeTree.isValid()) {
    restoredValues.reserve(static_cast<size_t>(runtimeTree.getNumChildren()));
    for (int i = 0; i < runtimeTree.getNumChildren(); ++i) {
      const auto paramTree = runtimeTree.getChild(i);
      if (!paramTree.hasType(kRuntimeParamStateType)) {
        continue;
      }
      RestoredRuntimeParameterValue restored;
      restored.index = static_cast<int>(paramTree.getProperty(kRuntimeParamIndexStateKey, -1));
      restored.name = paramTree.getProperty(kRuntimeParamNameStateKey).toString().toStdString();
      restored.value = static_cast<float>(
          static_cast<double>(paramTree.getProperty(kRuntimeParamValueStateKey)));
      if (!restored.name.empty() && std::isfinite(restored.value)) {
        restoredValues.push_back(std::move(restored));
      }
    }
  }

  const juce::String restoredPath = tree.getProperty(kModelPathStateKey).toString();
  if (restoredPath.isEmpty()) {
    StageModelClear();
  } else {
    const juce::File modelFile(restoredPath);
    if (modelFile.existsAsFile() && modelFile.hasReadAccess()) {
      StartModelLoad(modelFile, std::move(restoredValues));
    } else {
      std::lock_guard<std::mutex> lock(mLoadMutex);
      mStatusText = "Stored model path is unavailable: " + restoredPath;
    }
  }

  const juce::String restoredIrPath = tree.getProperty(kIrPathStateKey).toString();
  if (restoredIrPath.isEmpty()) {
    StageIrClear();
  } else {
    const juce::File irFile(restoredIrPath);
    if (irFile.existsAsFile() && irFile.hasReadAccess()) {
      StartIrLoad(irFile);
    } else {
      std::lock_guard<std::mutex> lock(mIrLoadMutex);
      mIrStatusText = "Stored IR path is unavailable: " + restoredIrPath;
    }
  }
}

void NamParametricPluginAudioProcessor::timerCallback() {
  CollectCompletedModelLoad();
  CollectCompletedIrLoad();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
  return new NamParametricPluginAudioProcessor();
}
