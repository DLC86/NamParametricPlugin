#include "DSP/NamModelEngine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numeric>
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
#include "wavenet/a2_fast.h"
#include "wavenet/hyperwavenet.h"

namespace namparametric::dsp {

namespace {
constexpr double kAssumedUnknownModelSampleRate = 48000.0;

// New "options" metadata is visual only. NAM Core still expects enum_names
// in the original neural value order, without changing the one-hot encoding.
void NormalizeSwitchOptions(nlohmann::json& config) {
  if (!config.is_object()) return;
  if (config.contains("params") && config["params"].is_array()) {
    for (auto& param : config["params"]) {
      if (!param.is_object() || !param.contains("options")) continue;
      if (param.value("type","")!="switch")
        throw std::runtime_error("Only switch controls may have options");
      const auto& options=param.at("options");
      if (!options.is_array() || options.size()<2)
        throw std::runtime_error("Switch options must be an array with at least two entries");
      std::vector<std::string> names(options.size());
      std::vector<bool> usedValue(options.size(),false),usedDisplay(options.size(),false);
      for (const auto& option: options) {
        if (!option.is_object() || !option.contains("name") || !option["name"].is_string()
            || !option.contains("value") || !option["value"].is_number_integer()
            || !option.contains("index_sw") || !option["index_sw"].is_number_integer())
          throw std::runtime_error("Each switch option requires name, value and index_sw");
        const int value=option["value"].get<int>(), display=option["index_sw"].get<int>();
        if (value<0 || display<0 || static_cast<size_t>(value)>=options.size() ||
            static_cast<size_t>(display)>=options.size() ||
            usedValue[static_cast<size_t>(value)] || usedDisplay[static_cast<size_t>(display)])
          throw std::runtime_error("Switch value/index_sw must be unique 0..N-1 permutations");
        names[static_cast<size_t>(value)]=option["name"].get<std::string>();
        usedValue[static_cast<size_t>(value)]=usedDisplay[static_cast<size_t>(display)]=true;
      }
      if (param.contains("enum_names") && !param["enum_names"].is_null() &&
          param["enum_names"] != names)
        throw std::runtime_error("Switch options disagree with enum_names");
      param["enum_names"]=names;
      param.erase("options");
    }
  }
  if (config.contains("submodels") && config["submodels"].is_array())
    for (auto& child: config["submodels"])
      if (child.is_object() && child.contains("config"))
        NormalizeSwitchOptions(child["config"]);
  if (config.contains("base_config"))
    NormalizeSwitchOptions(config["base_config"]);
}

size_t ReadCount(const nlohmann::json& entry, const char* key) {
  const auto& value = entry.at(key);
  if (!value.is_number_integer() || value < 0) {
    throw std::runtime_error(std::string("HyperA2: invalid ") + key);
  }
  return value.get<size_t>();
}

// The trainer exports Lite then Full, each with the unchanged HyperWaveNet
// layout. Select before constructing the DSP so only one audio network runs.
nlohmann::json SelectHyperA2Branch(const nlohmann::json& model, ModelSize size) {
  if (model.at("version") != "1.0.0") {
    throw std::runtime_error("HyperA2: unsupported model version (expected 1.0.0).");
  }
  const auto& config = model.at("config");
  const auto& weights = model.at("weights");
  const auto& branches = config.at("submodels");
  if (!weights.is_array() || !branches.is_array() || branches.size() != 2) {
    throw std::runtime_error("HyperA2: expected Lite and Full submodels and a flat weights array.");
  }
  nam::parse_param_specs(config, "HyperA2");
  size_t cursor = 0;
  for (size_t index = 0; index < 2; ++index) {
    const auto& branch = branches[index];
    const auto& branchConfig = branch.at("config");
    int channels = 0;
    const size_t count = ReadCount(branch, "weight_count");
    const size_t baseCount = ReadCount(branch, "base_weight_count");
    if (branch.at("name") != (index == 0 ? "lite" : "full") ||
        branch.at("architecture") != "HyperWaveNet" ||
        branch.at("max_value") != (index == 0 ? 0.5 : 1.0) ||
        branchConfig.at("params") != config.at("params") ||
        !nam::wavenet::a2_fast::is_a2_shape(branchConfig, &channels) ||
        channels != (index == 0 ? 3 : 8) || baseCount != (index == 0 ? 1871u : 12146u) ||
        count <= baseCount || ReadCount(branch, "weight_offset") != cursor ||
        cursor > weights.size() || count > weights.size() - cursor) {
      throw std::runtime_error(
          "HyperA2: inconsistent submodel topology, controls or weight layout.");
    }
    cursor += count;
  }
  if (cursor != weights.size()) {
    throw std::runtime_error("HyperA2: weights contain unowned trailing values.");
  }
  const auto& selected = branches[size == ModelSize::Lite ? 0 : 1];
  const size_t offset = ReadCount(selected, "weight_offset");
  const size_t count = ReadCount(selected, "weight_count");
  nlohmann::json child = model;
  child["architecture"] = "HyperWaveNet";
  child["config"] = selected.at("config");
  child["weights"] = nlohmann::json::array();
  for (size_t i = offset; i < offset + count; ++i) {
    child["weights"].push_back(weights[i]);
  }
  return child;
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
  auto normalizedSource=source;
  if (normalizedSource.contains("config"))
    NormalizeSwitchOptions(normalizedSource["config"]);
  if (const auto* gainConfig = pnam::OutputGainCurve::FindConfig(normalizedSource)) {
    auto base=pnam::OutputGainCurve::NeuralSource(normalizedSource);
    pnam::OutputGainCurve curve;
    curve.Load(*gainConfig,base.at("config").at("params").size());
    if (values.size()!=base.at("config").at("params").size()+1)
      throw std::runtime_error("Output-volume export control count mismatch");
    for (float value:values) if (!std::isfinite(value)) throw std::runtime_error("Nonfinite control value");
    if (values[curve.index]<curve.minimum || values[curve.index]>curve.maximum)
      throw std::runtime_error("Output-volume value outside range");
    std::vector<float> neural;
    for (size_t i=0;i<values.size();++i) if (i!=curve.index) neural.push_back(values[i]);
    auto baked=BakeStaticModel(base,neural,sourceFileName);
    const double gain=curve.Evaluate(values[curve.index]);
    const auto scale=[gain](nlohmann::json& model) {
      model["config"]["head_scale"]=model["config"].value("head_scale",1.0)*gain;
      model["metadata"]["nam_parametric_plugin"]["static_export"]["output_gain"] = gain;
    };
    if (baked.at("architecture")=="SlimmableContainer")
      for (auto& child:baked["config"]["submodels"]) scale(child["model"]);
    else scale(baked);
    auto checked=nam::get_dsp(baked);
    return baked;
  }
  const auto architecture = normalizedSource.at("architecture").get<std::string>();
  nlohmann::json result;
  if (architecture == "HyperWaveNet") {
    result = BakeWaveNet(normalizedSource, values);
  } else if (architecture == "HyperA2" ||
             architecture == "ParametricSlimmableContainer") {
    result = normalizedSource;
    result["version"] = nam::LATEST_FULLY_SUPPORTED_NAM_FILE_VERSION;
    result["architecture"] = "SlimmableContainer";
    result["config"] = {
        {"submodels",
         nlohmann::json::array(
             {{{"max_value", 0.5},
               {"model", BakeWaveNet(SelectHyperA2Branch(normalizedSource, ModelSize::Lite), values)}},
              {{"max_value", 1.0},
               {"model", BakeWaveNet(SelectHyperA2Branch(normalizedSource, ModelSize::Full), values)}}})}};
    result["weights"] = nlohmann::json::array();
  } else {
    throw std::runtime_error("Static export supports HyperWaveNet and parametric containers only.");
  }
  const auto specs = nam::parse_param_specs(normalizedSource.at("config"), architecture);
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
  if (architecture == "HyperA2" ||
      architecture == "ParametricSlimmableContainer")
    provenance["variants"] = {"lite", "full"};
  const auto addMetadata = [&provenance](nlohmann::json& model) {
    if (!model.contains("metadata") || !model["metadata"].is_object())
      model["metadata"] = nlohmann::json::object();
    auto& custom = model["metadata"]["nam_parametric_plugin"];
    if (!custom.is_object()) custom = nlohmann::json::object();
    custom["static_export"] = provenance;
  };
  addMetadata(result);
  if (architecture == "HyperA2" ||
      architecture == "ParametricSlimmableContainer") {
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


void AssignDisplayIndices(std::vector<ModelParameterInfo>& loadedParams,
                          const nlohmann::json& original, const pnam::OutputGainCurve& volume) {
  // The neural parameter array is positional: only the GUI may use the optional indices.
  for (size_t i = 0; i < loadedParams.size(); ++i) loadedParams[i].displayIndex = i;
  if (loadedParams.empty()) return;  // Legacy and baked static NAM files have no params.
  const auto base = pnam::OutputGainCurve::NeuralSource(original);
  const auto& stored = base.at("config").at("params");
  if (stored.size() + (volume.enabled ? 1u : 0u) != loadedParams.size())
    throw std::runtime_error("Display control count differs from neural controls");
  const bool indexed = std::any_of(stored.begin(), stored.end(),
      [](const nlohmann::json& entry) { return entry.contains("index"); });
  if (!indexed) return;  // Legacy models keep the historical neural + Volume order.
  std::vector<bool> seen(loadedParams.size(), false);
  size_t neuralIndex = 0;
  for (size_t native = 0; native < loadedParams.size(); ++native) {
    size_t display = native;
    if (!volume.enabled || native != volume.index) {
      const auto& entry = stored.at(neuralIndex++);
      if (!entry.contains("index") || !entry.at("index").is_number_integer())
        throw std::runtime_error("All neural controls must have integer display indices");
      const auto value = entry.at("index").get<int>();
      if (value < 0 || static_cast<size_t>(value) >= loadedParams.size())
        throw std::runtime_error("Neural display index out of range");
      display = static_cast<size_t>(value);
    }
    if (seen[display]) throw std::runtime_error("Duplicate control display index");
    seen[display] = true;
    loadedParams[native].displayIndex = display;
  }
}

bool NamModelEngine::LoadModel(const std::string& modelPath, std::string& errorMessage,
                               const ModelSize size) {
  errorMessage.clear();

  try {
    std::ifstream stream{std::filesystem::path(modelPath)};
    if (!stream) {
      throw std::runtime_error("Could not open model file.");
    }
    auto config = nlohmann::json::parse(stream);
    const auto original = config;
    NormalizeSwitchOptions(config["config"]);
    const auto originalNeural = pnam::OutputGainCurve::NeuralSource(original);
    const auto* gainConfig = pnam::OutputGainCurve::FindConfig(original);
    const bool gainWrapped = gainConfig != nullptr;
    config = pnam::OutputGainCurve::NeuralSource(config);
    pnam::OutputGainCurve outputGain;
    const auto architecture = config.at("architecture").get<std::string>();
    const bool hyperA2 = architecture == "HyperA2" ||
                         architecture == "ParametricSlimmableContainer";
    const bool staticA2 = IsStaticA2Container(config);
    const bool supportsSizeSelection = hyperA2 || staticA2;
    // Input/output calibration and gear type belong to the outer model.
    // Loudness alone may be specific to the selected Lite/Full branch.
    const auto rootMetadata = config.value("metadata", nlohmann::json::object());
    nlohmann::json branchMetadata = nlohmann::json::object();
    if (supportsSizeSelection) {
      const auto& entry = config.at("config").at("submodels").at(
          size == ModelSize::Lite ? 0 : 1);
      if (entry.contains("model") && entry["model"].is_object()) {
        branchMetadata = entry["model"].value("metadata", nlohmann::json::object());
      } else {
        branchMetadata = entry.value("metadata", nlohmann::json::object());
      }
      if (!branchMetadata.is_object()) branchMetadata = nlohmann::json::object();
    }
    std::shared_ptr<const nlohmann::json> exportSource;
    if (hyperA2 || config.at("architecture") == "HyperWaveNet")
      exportSource = std::make_shared<const nlohmann::json>(original);
    if (hyperA2) {
      config = SelectHyperA2Branch(config, size);
    } else if (staticA2) {
      // The saved file still contains both branches; playback constructs just the selected one.
      auto child = config.at("config").at("submodels")[size == ModelSize::Lite ? 0 : 1].at("model");
      nam::verify_config_version(config.at("version").get<std::string>());
      config = std::move(child);
    }
    if (supportsSizeSelection) {
      if (!config.contains("metadata") || !config["metadata"].is_object())
        config["metadata"] = nlohmann::json::object();
      auto& effectiveMetadata = config["metadata"];
      // Never inherit input/output calibration or gear_type from a child.
      // For a missing/invalid child loudness, use the outer loudness.
      for (const char* key : {"input_level_dbu", "output_level_dbu", "gear_type"}) {
        effectiveMetadata.erase(key);
        if (rootMetadata.is_object() && rootMetadata.contains(key))
          effectiveMetadata[key] = rootMetadata[key];
      }
      const auto validLoudness = [](const nlohmann::json& metadata) {
        return metadata.is_object() && metadata.contains("loudness") &&
               metadata["loudness"].is_number() &&
               std::isfinite(metadata["loudness"].get<double>());
      };
      effectiveMetadata.erase("loudness");
      if (validLoudness(branchMetadata))
        effectiveMetadata["loudness"] = branchMetadata["loudness"];
      else if (validLoudness(rootMetadata))
        effectiveMetadata["loudness"] = rootMetadata["loudness"];
    }
    auto loadedModel = nam::get_dsp(config);
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
          info.enumValues.resize(spec.enum_names.size());
          std::iota(info.enumValues.begin(), info.enumValues.end(), 0);
          const auto& raw = originalNeural.at("config").at("params").at(loadedParams.size());
          if (raw.contains("options")) {
            const auto originalNames = info.enumNames;
            for (const auto& option: raw.at("options")) {
              const size_t visual = option.at("index_sw").get<size_t>();
              const size_t neural = option.at("value").get<size_t>();
              info.enumNames.at(visual)=originalNames.at(neural);
              info.enumValues.at(visual)=static_cast<int>(neural);
            }
          }
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
          }
          break;
        }
      }
    }
    if (gainWrapped) {
      outputGain.Load(*gainConfig,loadedValues.size());
      for (const auto& p : loadedParams)
        if (p.name==outputGain.name) throw std::runtime_error("Output-volume control is already a neural parameter");
      loadedParams.insert(loadedParams.begin()+static_cast<std::ptrdiff_t>(outputGain.index),
        ModelParameterInfo{outputGain.name,static_cast<float>(outputGain.minimum),static_cast<float>(outputGain.maximum),static_cast<float>(outputGain.defaultValue),{}});
    }
    AssignDisplayIndices(loadedParams, original, outputGain);
    mOutputGain=std::move(outputGain);
    mGainTarget=mGainCurrent=mOutputGain.Evaluate(mOutputGain.defaultValue);
    mGainNeuralValues=loadedValues;
    mControlValues=loadedValues;
    if (mOutputGain.enabled) mControlValues.insert(mControlValues.begin()+static_cast<std::ptrdiff_t>(mOutputGain.index),mOutputGain.defaultValue);
    mModel = std::move(loadedModel);
    mRenderedValues = loadedValues;
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
  if (!IsLoaded()) return;
  mHostSampleRate = sampleRate;
  mGainAlpha = 1 - std::exp(-1 / (0.005 * sampleRate));
  mGainCurrent = mGainTarget;
  mMaxExternalBlockSize = maxBlockSize;
  const double modelRate = GetModelSampleRateForProcessing();
  mUpdateIntervalSamples = std::max(1, static_cast<int>(std::lround(modelRate * 0.100)));
  mUpdateCooldownRemaining = 0;
  if (mParametricControl)
    mParametricControl->SetParams(mParameterValues);
  mRenderedValues = mParameterValues;
  if (!NeedToResample()) {
    mResamplerState.reset();
    mModel->Reset(sampleRate, maxBlockSize);
  } else {
    mResamplerState = std::make_unique<ResamplerState>(modelRate);
    mResamplerState->blockProcessFunc = [this](NAM_SAMPLE** input, NAM_SAMPLE** output,
                                               int frames) {
      ProcessModel(input[0], output[0], frames);
    };
    mResamplerState->resampler.Reset(sampleRate, maxBlockSize);
    const double ratio = sampleRate / modelRate;
    const int innerMax = std::max(1, static_cast<int>(
        std::ceil(static_cast<double>(maxBlockSize) / ratio)));
    mModel->Reset(sampleRate, innerMax);
  }
  // Neural weights change linearly at every audio sample *within* the
  // convolution kernels; static/idle rendering stays on the original path.
  if (auto* hyper = dynamic_cast<nam::wavenet::HyperWaveNet*>(mModel.get()))
    hyper->ConfigureKernelSmoothing(
        std::max(2, static_cast<int>(std::lround(modelRate * 0.100))), 2);
}

void NamModelEngine::ProcessModel(float* input, float* output, int numSamples) {
  int offset = 0;
  while (offset < numSamples) {
    if (mParametricControl && mParameterValues != mRenderedValues &&
        mUpdateCooldownRemaining == 0) {
      mParametricControl->SetParams(mParameterValues);
      mRenderedValues = mParameterValues;
      mUpdateCooldownRemaining = mUpdateIntervalSamples;
    }
    int count = numSamples - offset;
    if (mParametricControl && mParameterValues != mRenderedValues &&
        mUpdateCooldownRemaining > 0)
      count = std::min(count, mUpdateCooldownRemaining);
    // A fixed update cadence uses model sample time and never allocates
    // or runs a second network while the controls move.
    auto* in = input + offset;
    auto* out = output + offset;
    mModel->process(&in, &out, count);
    mUpdateCooldownRemaining = std::max(0, mUpdateCooldownRemaining - count);
    offset += count;
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
    ApplyOutputGain(output,numSamples);
    return;
  }

  mResamplerState->resampler.ProcessBlock(inputPtrs, outputPtrs, numSamples,
                                          mResamplerState->blockProcessFunc);
  ApplyOutputGain(output,numSamples);
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

const std::vector<float>& NamModelEngine::GetParameterValues() const { return mOutputGain.enabled ? mControlValues : mParameterValues; }

void NamModelEngine::ApplyOutputGain(float* output,int samples) noexcept {
  if (!mOutputGain.enabled) return;
  for (int i=0;i<samples;++i) {
    mGainCurrent += mGainAlpha*(mGainTarget-mGainCurrent);
    if (std::abs(mGainTarget-mGainCurrent)<1e-9) mGainCurrent=mGainTarget;
    output[i] *= static_cast<float>(mGainCurrent);
  }
}

bool NamModelEngine::SetParameterValues(const std::span<const float> values) noexcept {
  if (!mParametricControl) return false;
  const auto expected = mOutputGain.enabled ? mControlValues.size() : mParameterValues.size();
  if (values.size() != expected || mParameterInfos.size() != expected) return false;
  for (size_t i=0;i<values.size();++i) {
    const auto& info=mParameterInfos[i];
    const float value=values[i];
    if (!std::isfinite(value) || value<info.minValue || value>info.maxValue ||
        (info.IsSwitch() && std::round(value)!=value))
      return false;
  }
  if (mOutputGain.enabled) {
    size_t n=0;
    for (size_t i=0;i<values.size();++i)
      if (i!=mOutputGain.index)
        mGainNeuralValues[n++]=values[i];
    std::copy(mGainNeuralValues.begin(),mGainNeuralValues.end(),mParameterValues.begin());
    std::copy(values.begin(),values.end(),mControlValues.begin());
    mGainTarget=mOutputGain.Evaluate(values[mOutputGain.index]);
  } else {
    std::copy(values.begin(),values.end(),mParameterValues.begin());
  }
  return true;
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
