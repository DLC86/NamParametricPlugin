#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dsp.h"
#include "parametric_control.h"
#include "DSP/OutputGainCurve.h"

namespace namparametric::dsp {

enum class ModelSize { Full, Lite };
enum class OutputMode { Raw, Normalized, Calibrated, Auto };
// HyperWaveNet uses one network with fixed 100 ms updates and
// per-sample linear coefficient interpolation over 100 ms.
struct ModelParameterInfo {
  std::string name;
  float minValue = 0.0f;
  float maxValue = 0.0f;
  float defaultValue = 0.0f;
  std::vector<std::string> enumNames; // index_sw display order
  std::vector<int> enumValues; // display position -> original neural switch value
  // GUI order only; original positional neural indices must never change.
  size_t displayIndex = 0;

  [[nodiscard]] bool IsSwitch() const { return !enumNames.empty(); }
};

class NamModelEngine {
 public:
  NamModelEngine();
  ~NamModelEngine();

  bool LoadModel(const std::string& modelPath, std::string& errorMessage,
                 ModelSize size = ModelSize::Full);
  [[nodiscard]] bool SupportsSizeSelection() const { return mSupportsSizeSelection; }
  [[nodiscard]] ModelSize GetModelSize() const { return mModelSize; }
  [[nodiscard]] std::shared_ptr<const nlohmann::json> GetExportSource() const {
    return mExportSource;
  }
  // Offline only: generate fixed weights without touching the live audio model.
  static nlohmann::json BakeStaticModel(const nlohmann::json& source, std::span<const float> values,
                                        const std::string& sourceFileName);
  [[nodiscard]] double GetInputCalibrationGainDb(double interfaceLevel, bool enabled) const;
  [[nodiscard]] double GetOutputCalibrationGainDb(double interfaceLevel, OutputMode mode) const;

  void Reset(double sampleRate, int maxBlockSize);
  void Process(float* input, float* output, int numSamples);

  [[nodiscard]] bool IsLoaded() const { return mModel != nullptr; }
  [[nodiscard]] double GetExpectedSampleRate() const;
  [[nodiscard]] const std::vector<ModelParameterInfo>& GetParameterInfos() const;
  [[nodiscard]] const std::vector<float>& GetParameterValues() const;
  bool SetParameterValues(std::span<const float> values) noexcept;
 
 private:
  struct ResamplerState;
  [[nodiscard]] double GetModelSampleRateForProcessing() const;
  [[nodiscard]] bool NeedToResample() const;
  void ProcessModel(float* input, float* output, int numSamples);
  pnam::OutputGainCurve mOutputGain;
  std::vector<float> mControlValues, mGainNeuralValues;
  double mGainTarget=1, mGainCurrent=1, mGainAlpha=1;
  void ApplyOutputGain(float* output,int samples) noexcept;
  std::unique_ptr<nam::DSP> mModel;
  // Only one NAM DSP network. No standby DSP, crossfade or output-step predictor.
  std::vector<float> mRenderedValues;
  int mUpdateIntervalSamples = 1, mUpdateCooldownRemaining = 0;
  std::shared_ptr<const nlohmann::json> mExportSource;
  bool mSupportsSizeSelection = false;
  ModelSize mModelSize = ModelSize::Full;
  std::string mGearType;
  nam::IParametricControl* mParametricControl = nullptr;
  std::vector<ModelParameterInfo> mParameterInfos;
  std::vector<float> mParameterValues;
  double mHostSampleRate = 48000.0;
  int mMaxExternalBlockSize = 0;
  std::unique_ptr<ResamplerState> mResamplerState;
};

}  // namespace namparametric::dsp
