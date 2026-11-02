#pragma once

#include <memory>
#include <span>
#include <string>
#include <vector>

#include "dsp.h"
#include "parametric_control.h"

namespace namparametric::dsp {

enum class ModelSize { Full, Lite };
enum class OutputMode { Raw, Normalized, Calibrated, Auto };

struct ModelParameterInfo {
  std::string name;
  float minValue = 0.0f;
  float maxValue = 0.0f;
  float defaultValue = 0.0f;
  std::vector<std::string> enumNames;

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

  std::unique_ptr<nam::DSP> mModel;
  // Both histories keep receiving the same audio. Only the inaudible network
  // receives new weights; a sample-continuous fade then exchanges their roles.
  std::unique_ptr<nam::DSP> mTransitionModel;
  nam::IParametricControl* mTransitionControl = nullptr;
  std::vector<float> mTransitionOutput, mRenderedValues;
  int mFadePosition = 0;
  int mFadeLength = 0;
  bool mFading = false;
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
