#include "DSP/NamModelEngine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <stdexcept>

namespace iplug {
// AudioDSPTools resampling code assumes this is an iPlug plugin
inline constexpr double PI = std::numbers::pi_v<double>;
}  // namespace iplug

#ifndef DEFAULT_BLOCK_SIZE
#define DEFAULT_BLOCK_SIZE 1024
#endif

#include "dsp/ResamplingContainer/ResamplingContainer.h"
#include "get_dsp.h"
#include "wavenet/hyper_a2.h"
#include "wavenet/hyperwavenet.h"

namespace namparametric::dsp {

namespace {
constexpr double kAssumedUnknownModelSampleRate = 48000.0;

nlohmann::json SelectHyperA2Branch(const nlohmann::json& model, ModelSize size) {
  return nam::wavenet::SelectHyperA2Branch(model, size == ModelSize::Lite);
}

bool IsStaticA2Container(const nlohmann::json& model) {
  if (model.at("architecture") != "SlimmableContainer") return false;
  const auto& children = model.at("config").at("submodels");
  if (!children.is_array() || children.size() != 2) return false;
  for (size_t index = 0; index < 2; ++index) {
    const auto& entry = children[index];
    const auto& child = entry.at("model");
    int channels = 0;
    if (entry.at("max_value") != (index == 0 ? 0.5 : 1.0) ||
        child.at("architecture") != "WaveNet" ||
        !nam::wavenet::a2_fast::is_a2_shape(child.at("config"), &channels) ||
        channels != (index == 0 ? 3 : 8))
      return false;
  }
  return true;
}

nlohmann::json BakeWaveNet(const nlohmann::json& source, std::span<const float> values) {
  // Use the same parser, range validation and hypernetwork implementation as playback.
  // Construction also checks the complete base-weight layout before ApplyConditioning.
  auto validated = nam::get_dsp(source);
  auto parsed = nam::wavenet::create_hyperwavenet_config(source.at("config"),
                                                         validated->GetExpectedSampleRate());
  const auto& config = dynamic_cast<const nam::wavenet::HyperWaveNetConfig&>(*parsed);
  const auto weights = source.at("weights").get<std::vector<float>>();
  const auto stateCount = nam::Hypernetwork::SerializedStateCount(config.hypernet, config.params);
  const auto baseCount = weights.size() - stateCount;
  const std::vector<float> base(weights.begin(),
                                weights.begin() + static_cast<std::ptrdiff_t>(baseCount));
  nam::Hypernetwork hypernet(config.hypernet, config.params,
                             std::span<const float>(weights).subspan(baseCount));
  hypernet.ValidateParams(values);
  for (size_t i = 0; i < values.size(); ++i) {
    if (values[i] < config.params[i].min || values[i] > config.params[i].max)
      throw std::runtime_error("Control is outside its model range: " + config.params[i].name);
  }
  auto conditioned = base;
  hypernet.ApplyConditioning(base, values, conditioned);
  if (!std::all_of(conditioned.begin(), conditioned.end(),
                   [](float value) { return std::isfinite(value); }))
    throw std::runtime_error("The generated model contains non-finite weights.");
  auto result = source;
  result["version"] = nam::LATEST_FULLY_SUPPORTED_NAM_FILE_VERSION;
  result["architecture"] = "WaveNet";
  result["config"].erase("params");
  result["config"].erase("hypernet");
  result["weights"] = conditioned;
  return result;
}
}  // namespace

nlohmann::json NamModelEngine::BakeStaticModel(const nlohmann::json& source,
                                               const std::span<const float> values,
                                               const std::string& sourceFileName) {
  const auto architecture = source.at("architecture").get<std::string>();
  nlohmann::json result;
  if (architecture == "HyperWaveNet") {
    result = BakeWaveNet(source, values);
  } else if (architecture == "HyperA2") {
    result = source;
    result["version"] = nam::LATEST_FULLY_SUPPORTED_NAM_FILE_VERSION;
    result["architecture"] = "SlimmableContainer";
    result["config"] = {
        {"submodels",
         nlohmann::json::array(
             {{{"max_value", 0.5},
               {"model", BakeWaveNet(SelectHyperA2Branch(source, ModelSize::Lite), values)}},
              {{"max_value", 1.0},
               {"model", BakeWaveNet(SelectHyperA2Branch(source, ModelSize::Full), values)}}})}};
    result["weights"] = nlohmann::json::array();
  } else {
    throw std::runtime_error("Static export supports HyperWaveNet and HyperA2 models only.");
  }
  const auto specs = nam::parse_param_specs(source.at("config"), architecture);
  nlohmann::json controls = nlohmann::json::array();
  for (size_t i = 0; i < specs.size(); ++i) {
    nlohmann::json control = {
        {"name", specs[i].name}, {"type", specs[i].type}, {"value", values[i]}};
    if (specs[i].type == "switch")
      control["label"] = specs[i].enum_names.at(static_cast<size_t>(values[i]));
    controls.push_back(std::move(control));
  }
  nlohmann::json provenance = {{"schema_version", 1},
                               {"source_file", sourceFileName},
                               {"source_architecture", architecture},
                               {"controls", controls}};
  if (architecture == "HyperA2") provenance["variants"] = {"lite", "full"};
  const auto addMetadata = [&provenance](nlohmann::json& model) {
    if (!model.contains("metadata") || !model["metadata"].is_object())
      model["metadata"] = nlohmann::json::object();
    auto& custom = model["metadata"]["nam_parametric_plugin"];
    if (!custom.is_object()) custom = nlohmann::json::object();
    custom["static_export"] = provenance;
  };
  addMetadata(result);
  if (architecture == "HyperA2") {
    for (auto& entry : result["config"]["submodels"]) addMetadata(entry["model"]);
  }
  // Validate the stock file before any destination is created or replaced.
  auto validated = nam::get_dsp(result);
  return result;
}

struct NamModelEngine::ResamplerState {
  explicit ResamplerState(double renderingSampleRate) : resampler(renderingSampleRate) {}

  std::function<void(NAM_SAMPLE**, NAM_SAMPLE**, int)> blockProcessFunc;
  ::dsp::ResamplingContainer<NAM_SAMPLE, 1, 12> resampler;
};

NamModelEngine::NamModelEngine() = default;
NamModelEngine::~NamModelEngine() = default;

bool NamModelEngine::LoadModel(const std::string& modelPath, std::string& errorMessage,
                               const ModelSize size) {
  errorMessage.clear();

  try {
    std::ifstream stream{std::filesystem::path(modelPath)};
    if (!stream) {
      throw std::runtime_error("Could not open model file.");
    }
    auto config = nlohmann::json::parse(stream);
    const bool hyperA2 = config.at("architecture") == "HyperA2";
    const bool staticA2 = IsStaticA2Container(config);
    const bool supportsSizeSelection = hyperA2 || staticA2;
    std::shared_ptr<const nlohmann::json> exportSource;
    if (hyperA2 || config.at("architecture") == "HyperWaveNet")
      exportSource = std::make_shared<const nlohmann::json>(config);
    if (hyperA2) {
      config = SelectHyperA2Branch(config, size);
    } else if (staticA2) {
      // The saved file still contains both branches; playback constructs just the selected one.
      auto child = config.at("config").at("submodels")[size == ModelSize::Lite ? 0 : 1].at("model");
      nam::verify_config_version(config.at("version").get<std::string>());
      config = std::move(child);
    }
    auto loadedModel = nam::get_dsp(config);
    auto transitionModel = exportSource ? nam::get_dsp(config) : nullptr;
    if (loadedModel == nullptr) {
      errorMessage = "Model loader returned null model.";
      return false;
    }
    if (loadedModel->NumInputChannels() != 1 || loadedModel->NumOutputChannels() != 1) {
      errorMessage = "Model must have exactly one input and one output channel.";
      return false;
    }

    auto* loadedParametricControl = dynamic_cast<nam::IParametricControl*>(loadedModel.get());
    std::vector<ModelParameterInfo> loadedParams;
    std::vector<float> loadedValues;

    if (loadedParametricControl != nullptr) {
      const auto& specs = loadedParametricControl->GetParamSpecs();
      const auto values = loadedParametricControl->GetParams();
      if (values.size() != specs.size()) {
        throw std::runtime_error("Parametric model returned inconsistent parameter metadata.");
      }

      loadedParams.reserve(specs.size());
      loadedValues.assign(values.begin(), values.end());
      for (const auto& spec : specs) {
        ModelParameterInfo info;
        info.name = spec.name;
        info.minValue = spec.min;
        info.maxValue = spec.max;
        info.defaultValue = spec.defaultValue;
        if (spec.type == "switch") {
          info.enumNames = spec.enum_names;
        }
        loadedParams.push_back(std::move(info));
      }
    }

    std::string gearType;
    if (config.contains("metadata") && config["metadata"].is_object() &&
        config["metadata"].contains("gear_type") && config["metadata"]["gear_type"].is_string()) {
      gearType = config["metadata"]["gear_type"].get<std::string>();
      std::transform(gearType.begin(), gearType.end(), gearType.begin(),
                     [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    }
    // Match NAM Oversampler's legacy return-level metadata fallback.
    if (!loadedModel->HasOutputLevel() && config.contains("metadata") &&
        config["metadata"].is_object()) {
      for (const auto* key : {"return_level", "return_level_dbu"}) {
        const auto& metadata = config["metadata"];
        if (metadata.contains(key) && metadata[key].is_number()) {
          const double level = metadata[key].get<double>();
          if (std::isfinite(level)) {
            loadedModel->SetOutputLevel(level);
            if (transitionModel) transitionModel->SetOutputLevel(level);
          }
          break;
        }
      }
    }
    mModel = std::move(loadedModel);
    mTransitionModel = std::move(transitionModel);
    mTransitionControl = dynamic_cast<nam::IParametricControl*>(mTransitionModel.get());
    mRenderedValues = loadedValues;
    mFading = false;
    mExportSource = std::move(exportSource);
    mGearType = std::move(gearType);
    mSupportsSizeSelection = supportsSizeSelection;
    mModelSize = size;
    mParametricControl = loadedParametricControl;
    mParameterInfos = std::move(loadedParams);
    mParameterValues = std::move(loadedValues);
    mResamplerState.reset();
    return true;
  } catch (const std::exception& ex) {
    errorMessage = ex.what();
    return false;
  }
}

void NamModelEngine::Reset(double sampleRate, int maxBlockSize) {
  if (!IsLoaded()) {
    return;
  }

  mHostSampleRate = sampleRate;
  mMaxExternalBlockSize = maxBlockSize;
  mFadeLength =
      std::max(2, static_cast<int>(std::lround(GetModelSampleRateForProcessing() * 0.050)));
  mFadePosition = 0;
  mFading = false;
  mActiveTransition = mRequestedTransition;
  mStandbyWarmup = 0;
  const double rate = GetModelSampleRateForProcessing();
  mCorrectionLength = std::max(2, static_cast<int>(std::lround(rate * 0.005)));
  mCorrectionPosition = mCorrectionLength;
  mRawHistory = mPredictionSkip = 0;
  mOutputHistory = 0;
  mLastOutput = mPreviousOutput = 0.0f;
  mCorrectionStart = mLastRaw = mPreviousRaw = mPredictionError = mRawPeak = 0.0f;
  mErrorDecay = static_cast<float>(std::exp(-1.0 / (rate * 0.010)));
  mPeakDecay = static_cast<float>(std::exp(-1.0 / (rate * 0.050)));
  if (mTransitionControl != nullptr) {
    mParametricControl->SetParams(mParameterValues);
    mTransitionControl->SetParams(mParameterValues);
    std::copy(mParameterValues.begin(), mParameterValues.end(), mRenderedValues.begin());
  }

  if (!NeedToResample()) {
    mResamplerState.reset();
    mModel->Reset(sampleRate, maxBlockSize);
    if (mTransitionModel) {
      mTransitionOutput.resize(static_cast<size_t>(maxBlockSize));
      mTransitionModel->Reset(sampleRate, maxBlockSize);
    }
    return;
  }

  const double modelSampleRate = GetModelSampleRateForProcessing();
  mResamplerState = std::make_unique<ResamplerState>(modelSampleRate);
  mResamplerState->blockProcessFunc = [this](NAM_SAMPLE** input, NAM_SAMPLE** output,
                                             int numFrames) {
    ProcessModel(input[0], output[0], numFrames);
  };
  mResamplerState->resampler.Reset(sampleRate, maxBlockSize);

  // Match NeuralAmpModelerPlugin block-size shaping for the encapsulated model.
  const double upRatio = sampleRate / modelSampleRate;
  const int maxEncapsulatedBlockSize =
      std::max(1, static_cast<int>(std::ceil(static_cast<double>(maxBlockSize) / upRatio)));
  mModel->Reset(sampleRate, maxEncapsulatedBlockSize);
  if (mTransitionModel) {
    mTransitionOutput.resize(static_cast<size_t>(maxEncapsulatedBlockSize));
    mTransitionModel->Reset(sampleRate, maxEncapsulatedBlockSize);
  }
}

void NamModelEngine::ProcessModel(float* input, float* output, int numSamples) {
  if (numSamples <= 0) return;
  ProcessModelInternal(input, output, numSamples);
  if (numSamples >= 2) {
    mPreviousOutput = output[numSamples - 2];
    mLastOutput = output[numSamples - 1];
    mOutputHistory = 2;
  } else if (numSamples == 1) {
    mPreviousOutput = mLastOutput;
    mLastOutput = output[0];
    mOutputHistory = std::min(2, mOutputHistory + 1);
  }
}

void NamModelEngine::ProcessModelInternal(float* input, float* output, int numSamples) {
  if (!mTransitionModel) {
    mModel->process(&input, &output, numSamples);
    return;
  }
  if (mRequestedTransition != mActiveTransition && !mFading &&
      mCorrectionPosition >= mCorrectionLength) {
    mActiveTransition = mRequestedTransition;
    mRawHistory = mPredictionSkip = 0;
    mPredictionError = mRawPeak = 0.0f;
    mStandbyWarmup = 0;
    if (mActiveTransition == ControlTransition::Eco) {
      mLastRaw = mLastOutput;
      mPreviousRaw = mPreviousOutput;
      mRawHistory = mOutputHistory;
      mRawPeak = std::max(std::abs(mLastRaw), std::abs(mPreviousRaw));
      mPredictionError = 2.0f * std::abs(mLastRaw - mPreviousRaw);
    }
    if (mActiveTransition == ControlTransition::Crossfade) {
      // The standby was asleep in Eco. Rebuild its history inaudibly at the
      // current controls before letting it take part in a parameter crossfade.
      mTransitionControl->SetParams(mRenderedValues);
      mStandbyWarmup = std::max(1, mTransitionModel->GetPrewarmSamples());
    }
  }
  if (mActiveTransition == ControlTransition::Eco) {
    ProcessEco(input, output, numSamples);
    return;
  }
  int offset = 0;
  while (offset < numSamples) {
    if (!mFading && mRequestedTransition == ControlTransition::Eco) {
      // Finish the current fade, then hand over on the next call. Do not
      // start another queued fade and starve a mode change during automation.
      auto* in = input + offset;
      auto* out = output + offset;
      mModel->process(&in, &out, numSamples - offset);
      return;
    }
    if (mStandbyWarmup > 0) {
      const int count = std::min(numSamples - offset, mStandbyWarmup);
      auto* in = input + offset;
      auto* out = output + offset;
      auto* alternate = mTransitionOutput.data();
      mTransitionModel->process(&in, &alternate, count);
      mModel->process(&in, &out, count);
      mStandbyWarmup -= count;
      offset += count;
      continue;
    }
    if (!mFading && mParameterValues != mRenderedValues) {
      mTransitionControl->SetParams(mParameterValues);
      std::copy(mParameterValues.begin(), mParameterValues.end(), mRenderedValues.begin());
      mFadePosition = 0;
      mFading = true;
    }
    const int count =
        mFading ? std::min(numSamples - offset, mFadeLength - mFadePosition) : numSamples - offset;
    auto* in = input + offset;
    auto* out = output + offset;
    auto* alternate = mTransitionOutput.data();
    // Standby first also makes an in-place Process(input, input) safe.
    mTransitionModel->process(&in, &alternate, count);
    mModel->process(&in, &out, count);
    if (mFading) {
      for (int i = 0; i < count; ++i) {
        const float t = static_cast<float>(mFadePosition + i) / (mFadeLength - 1);
        const float mix = t * t * (3.0f - 2.0f * t);
        out[i] = (1.0f - mix) * out[i] + mix * alternate[i];
      }
      mFadePosition += count;
      if (mFadePosition == mFadeLength) {
        mModel.swap(mTransitionModel);
        std::swap(mParametricControl, mTransitionControl);
        mFading = false;
      }
    }
    offset += count;
  }
}

void NamModelEngine::ProcessEco(float* input, float* output, int numSamples) {
  const bool changed =
      mRequestedTransition == ControlTransition::Eco && mParameterValues != mRenderedValues;
  if (changed) {
    mParametricControl->SetParams(mParameterValues);
    std::copy(mParameterValues.begin(), mParameterValues.end(), mRenderedValues.begin());
  }
  mModel->process(&input, &output, numSamples);
  for (int i = 0; i < numSamples; ++i) {
    const float raw = output[i];
    const float prediction = 2.0f * mLastRaw - mPreviousRaw;
    const float peak = std::max(mRawPeak, std::abs(raw));
    if (changed && i == 0) {
      if (mRawHistory >= 2) {
        const float jump = raw - prediction;
        // Ignore residuals within the signal's ordinary prediction error. This
        // avoids treating the curvature of an unchanged high-frequency tone as
        // a control click. Never feed the corrected audio back into this detector.
        const float excess = std::max(0.0f, std::abs(jump) - 2.0f * mPredictionError);
        if (excess > 1e-12f) {
          float tail = 0.0f;
          if (mCorrectionPosition < mCorrectionLength) {
            const float t = static_cast<float>(mCorrectionPosition) / (mCorrectionLength - 1);
            tail = mCorrectionStart * (1.0f - t * t * (3.0f - 2.0f * t));
          }
          mCorrectionStart =
              std::clamp(tail - std::copysign(excess, jump), -2.0f * peak, 2.0f * peak);
          mCorrectionPosition = 0;
        }
      }
      mPredictionSkip = 2;
    }
    if (mPredictionSkip > 0)
      --mPredictionSkip;
    else if (mRawHistory >= 2)
      mPredictionError = std::max(std::abs(raw - prediction), mPredictionError * mErrorDecay);
    mRawPeak = std::max(std::abs(raw), mRawPeak * mPeakDecay);
    if (mCorrectionPosition < mCorrectionLength) {
      const float t = static_cast<float>(mCorrectionPosition++) / (mCorrectionLength - 1);
      const float correction = mCorrectionStart * (1.0f - t * t * (3.0f - 2.0f * t));
      // Only bound the correction transient; raw model audio is never limited.
      output[i] = std::clamp(raw + correction, -peak, peak);
    }
    mPreviousRaw = mLastRaw;
    mLastRaw = raw;
    mRawHistory = std::min(2, mRawHistory + 1);
  }
}

void NamModelEngine::Process(float* input, float* output, int numSamples) {
  if (numSamples <= 0) {
    return;
  }

  if (!IsLoaded() || mMaxExternalBlockSize <= 0 || numSamples > mMaxExternalBlockSize) {
    std::copy_n(input, numSamples, output);
    return;
  }

  NAM_SAMPLE* inputPtrs[1] = {input};
  NAM_SAMPLE* outputPtrs[1] = {output};

  if (!NeedToResample() || mResamplerState == nullptr) {
    ProcessModel(input, output, numSamples);
    return;
  }

  mResamplerState->resampler.ProcessBlock(inputPtrs, outputPtrs, numSamples,
                                          mResamplerState->blockProcessFunc);
}

double NamModelEngine::GetExpectedSampleRate() const {
  if (!IsLoaded()) {
    return -1.0;
  }

  return GetModelSampleRateForProcessing();
}

double NamModelEngine::GetInputCalibrationGainDb(double interfaceLevel, bool enabled) const {
  return enabled && mModel != nullptr && mModel->HasInputLevel()
             ? interfaceLevel - mModel->GetInputLevel()
             : 0.0;
}

double NamModelEngine::GetOutputCalibrationGainDb(double interfaceLevel, OutputMode mode) const {
  if (mModel == nullptr) return 0.0;
  if (mode == OutputMode::Auto) {
    const bool amp = mGearType == "amp" || mGearType == "pedal_amp" || mGearType == "amp_cab" ||
                     mGearType == "amp_pedal_cab";
    mode = amp ? OutputMode::Normalized : OutputMode::Calibrated;
  }
  if (mode == OutputMode::Normalized && mModel->HasLoudness()) {
    return -18.0 - mModel->GetLoudness();
  }
  if (mode == OutputMode::Calibrated && mModel->HasOutputLevel()) {
    return mModel->GetOutputLevel() - interfaceLevel;
  }
  return 0.0;
}

const std::vector<ModelParameterInfo>& NamModelEngine::GetParameterInfos() const {
  return mParameterInfos;
}

const std::vector<float>& NamModelEngine::GetParameterValues() const { return mParameterValues; }

bool NamModelEngine::SetParameterValues(const std::span<const float> values) noexcept {
  if (mParametricControl == nullptr || values.size() != mParameterValues.size()) {
    return false;
  }
  try {
    if (mTransitionControl != nullptr) {
      // Validate before publishing a destination. Never interpolate switch indices
      // or their one-hot encoding; each network sees an actual switch position.
      for (size_t i = 0; i < values.size(); ++i) {
        if (!std::isfinite(values[i])) return false;
        const auto& info = mParameterInfos[i];
        if (info.IsSwitch() && (std::round(values[i]) != values[i] || values[i] < 0 ||
                                values[i] >= static_cast<float>(info.enumNames.size())))
          return false;
      }
    } else {
      mParametricControl->SetParams(values);
    }
    std::copy(values.begin(), values.end(), mParameterValues.begin());
    return true;
  } catch (...) {
    return false;
  }
}

double NamModelEngine::GetModelSampleRateForProcessing() const {
  if (!IsLoaded()) {
    return -1.0;
  }

  const double reportedRate = mModel->GetExpectedSampleRate();
  return reportedRate > 0.0 ? reportedRate : kAssumedUnknownModelSampleRate;
}

bool NamModelEngine::NeedToResample() const {
  if (!IsLoaded()) {
    return false;
  }

  constexpr double epsilon = 0.5;
  return std::abs(mHostSampleRate - GetModelSampleRateForProcessing()) > epsilon;
}

}  // namespace namparametric::dsp
