#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "DSP/NamModelEngine.h"
#include "json.hpp"

namespace {
using namparametric::dsp::ModelSize;
using namparametric::dsp::NamModelEngine;

void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

nlohmann::json Read(const std::filesystem::path& path) {
  std::ifstream file(path);
  return nlohmann::json::parse(file);
}

std::vector<float> Render(NamModelEngine& engine, const std::vector<float>& input,
                          double sampleRate = 48000.0) {
  constexpr int blockSize = 257;
  engine.Reset(sampleRate, blockSize);
  auto copy = input;
  std::vector<float> output(input.size());
  for (size_t offset = 0; offset < input.size(); offset += blockSize) {
    engine.Process(copy.data() + offset, output.data() + offset,
                   static_cast<int>(std::min<size_t>(blockSize, input.size() - offset)));
  }
  Check(std::all_of(output.begin(), output.end(), [](float value) { return std::isfinite(value); }),
        "Non-finite HyperA2 audio");
  return output;
}

void Compare(const std::vector<float>& actual, const std::vector<float>& expected) {
  Check(actual.size() == expected.size(), "Unexpected output length");
  double maximum = 0;
  for (size_t i = 0; i < actual.size(); ++i) {
    maximum = std::max(maximum, std::abs(static_cast<double>(actual[i]) - expected[i]));
  }
  Check(maximum < 2e-5, "HyperA2 disagrees with PyTorch; max error=" + std::to_string(maximum));
  std::cout << "PyTorch parity max_error=" << maximum << '\n';
}

void TestMalformedFiles(const std::filesystem::path& directory, const nlohmann::json& original) {
  for (int fault = 0; fault < 6; ++fault) {
    auto invalid = original;
    auto& branch = invalid["config"]["submodels"][0];
    switch (fault) {
      case 0:
        branch["weight_offset"] = -1;
        break;
      case 1:
        branch["weight_count"] = original["weights"].size() + 1;
        break;
      case 2:
        branch["config"]["params"][0]["name"] = "Wrong";
        break;
      case 3:
        branch["config"]["layers"][0]["channels"] = 4;
        break;
      case 4:
        invalid["weights"].push_back(0.0);
        break;
      case 5:
        invalid["version"] = "2.0.0";
        break;
    }
    const auto path = directory / ("invalid_" + std::to_string(fault) + ".nam");
    {
      std::ofstream file(path);
      file << invalid;
    }
    NamModelEngine engine;
    std::string error;
    Check(engine.LoadModel((directory / "hyper_a2_low_rank.nam").string(), error), error);
    Check(!engine.LoadModel(path.string(), error), "Malformed HyperA2 was accepted");
    Check(!error.empty() && engine.IsLoaded() && engine.SupportsSizeSelection(),
          "Failed load replaced the previous working model");
  }
}

void TestCalibration(const std::filesystem::path& directory) {
  using namparametric::dsp::OutputMode;
  auto model = Read(directory / "hyper_a2_low_rank.nam");
  model["metadata"]["input_level_dbu"] = 18.0;
  model["metadata"]["output_level_dbu"] = 6.0;
  model["metadata"]["loudness"] = -24.0;
  const auto path = directory / "calibration.nam";
  for (const auto gear : {"amp", "studio", ""}) {
    model["metadata"]["gear_type"] = gear;
    {
      std::ofstream file(path);
      file << model;
    }
    for (const auto size : {ModelSize::Full, ModelSize::Lite}) {
      NamModelEngine engine;
      std::string error;
      Check(engine.LoadModel(path.string(), error, size), error);
      Check(engine.GetInputCalibrationGainDb(12, true) == -6, "Incorrect input calibration sign");
      Check(engine.GetInputCalibrationGainDb(12, false) == 0, "Input bypass failed");
      Check(engine.GetInputCalibrationGainDb(18, true) == 0,
            "Matched input levels should be unity");
      Check(engine.GetOutputCalibrationGainDb(12, OutputMode::Raw) == 0, "Raw output is not unity");
      Check(engine.GetOutputCalibrationGainDb(12, OutputMode::Normalized) == 6,
            "Wrong normalized output");
      Check(engine.GetOutputCalibrationGainDb(12, OutputMode::Calibrated) == -6,
            "Wrong calibrated output sign");
      Check(engine.GetOutputCalibrationGainDb(12, OutputMode::Auto) ==
                (std::string(gear) == "amp" ? 6 : -6),
            "Auto mode does not match standard NAM gear behavior");
    }
  }
  model["metadata"] = nlohmann::json::object();
  {
    std::ofstream file(path);
    file << model;
  }
  NamModelEngine engine;
  std::string error;
  Check(engine.LoadModel(path.string(), error), error);
  Check(engine.GetInputCalibrationGainDb(12, true) == 0 &&
            engine.GetOutputCalibrationGainDb(12, OutputMode::Calibrated) == 0 &&
            engine.GetOutputCalibrationGainDb(12, OutputMode::Normalized) == 0,
        "Missing metadata must not invent a calibration");
}
}  // namespace

int main(int argc, const char* const* argv) {
  try {
    Check(argc == 2, "Usage: hyper_a2_engine_test <fixture-directory>");
    const std::filesystem::path directory(argv[1]);
    const auto manifest = Read(directory / "manifest.json");
    const auto input = manifest.at("input").get<std::vector<float>>();
    for (const auto& testCase : manifest.at("cases")) {
      const auto path = directory / testCase.at("model").get<std::string>();
      NamModelEngine engine;
      std::string error;
      Check(engine.LoadModel(path.string(), error), error);
      Check(engine.SupportsSizeSelection() && engine.GetModelSize() == ModelSize::Full,
            "HyperA2 must default to Full, even when the trainer export prefers Lite");
      Check(engine.GetParameterInfos().size() == 2 && engine.GetParameterInfos()[1].IsSwitch(),
            "Missing continuous/switch controls");
      for (const auto size : {ModelSize::Full, ModelSize::Lite}) {
        Check(engine.LoadModel(path.string(), error, size), error);
        const auto name = size == ModelSize::Full ? "full" : "lite";
        for (size_t index = 0; index < 2; ++index) {
          const auto params = manifest.at("params")[index].get<std::vector<float>>();
          Check(engine.SetParameterValues(params), "Control update was rejected");
          Compare(Render(engine, input),
                  testCase.at("expected").at(name)[index].get<std::vector<float>>());
          Render(engine, input, 44100.0);  // Resampling and odd buffer sizes in both variants.
        }
      }
    }
    TestMalformedFiles(directory, Read(directory / "hyper_a2_low_rank.nam"));
    TestCalibration(directory);
    std::cout << "HyperA2 Full/Lite engine tests passed\n";
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
