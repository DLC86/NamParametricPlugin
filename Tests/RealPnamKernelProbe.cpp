// Private local validation helper: compiled by CI, run on the user's own
// .pnam file without uploading its contents into a public GitHub repository.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "get_dsp.h"
#include "parametric_control.h"
#include "wavenet/hyperwavenet.h"

using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr int kRate = 48000;
constexpr int kBlock = 257;
constexpr int kPreroll = 16000;
constexpr int kOutput = 9600;
constexpr int kRamp = 1920;
constexpr int kRetargetInterval = 960;

void Require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}

std::string ModeName(int step) {
  if (step == -1) return "unchanged";
  if (step == 0) return "instant";
  if (step == 1) return "reload_sample";
  if (step == -2) return "kernel_sample";
  return "step_" + std::to_string(step);
}

struct Result {
  std::vector<float> samples;
  double elapsedMs = 0.0;
  double maxBlockMs = 0.0;
};

std::vector<float> MakeInput(bool tone) {
  std::vector<float> in(kPreroll + kOutput, 0.f);
  if (tone) {
    for (size_t i = 0; i < in.size(); ++i) {
      const double t = static_cast<double>(i) / kRate;
      in[i] = static_cast<float>(
          0.13 * std::sin(2 * std::numbers::pi * 440.0 * t) +
          0.07 * std::sin(2 * std::numbers::pi * 3197.0 * t));
    }
  }
  return in;
}

Result Run(const Json& config, const std::vector<float>& target,
           const std::vector<float>& signal, int step, bool automate) {
  auto dsp = nam::get_dsp(config);
  auto* hyper = dynamic_cast<nam::wavenet::HyperWaveNet*>(dsp.get());
  auto* control = dynamic_cast<nam::IParametricControl*>(dsp.get());
  Require(hyper && control, "Expected a direct HyperWaveNet model");
  Require(dsp->NumInputChannels() == 1 && dsp->NumOutputChannels() == 1,
          "Expected mono amplifier model");
  const auto defaultsSpan = control->GetParams();
  const std::vector<float> defaults(defaultsSpan.begin(), defaultsSpan.end());
  dsp->SetPrewarmOnReset(false);
  dsp->Reset(kRate, kBlock);
  std::vector<float> pre(kPreroll);
  for (int i = 0; i < kPreroll; i += kBlock) {
    int count = std::min(kBlock, kPreroll - i);
    float* in = const_cast<float*>(signal.data() + i);
    float* out = pre.data() + i;
    dsp->process(&in, &out, count);
  }

  hyper->ConfigureWeightSmoothing((step == -2 || step > 0) ? kRamp : 0,
                                  step == -2 ? 0 : std::max(1, step));
  if (!automate && step != -1) control->SetParams(target);

  Result result;
  result.samples.resize(kOutput);
  for (int offset = 0; offset < kOutput;) {
    int frames = std::min(kBlock, kOutput - offset);
    if (automate && offset < 5 * kRetargetInterval)
      frames = std::min(frames, kRetargetInterval - offset % kRetargetInterval);
    const auto start = std::chrono::steady_clock::now();
    if (automate && offset < 5 * kRetargetInterval &&
        offset % kRetargetInterval == 0 && step != -1) {
      const int move = offset / kRetargetInterval;
      control->SetParams((move % 2 == 0) ? target : defaults);
    }
    float* in = const_cast<float*>(signal.data() + kPreroll + offset);
    float* out = result.samples.data() + offset;
    dsp->process(&in, &out, frames);
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    result.elapsedMs += ms;
    result.maxBlockMs = std::max(result.maxBlockMs, ms);
    offset += frames;
  }
  Require(std::all_of(result.samples.begin(), result.samples.end(),
                      [](float sample) {return std::isfinite(sample);}),
          "Non-finite model output");
  return result;
}

std::vector<float> ParseParams(const std::string& csv) {
  std::vector<float> values;
  std::istringstream input(csv);
  std::string token;
  while (std::getline(input, token, ',')) {
    if (token.empty()) throw std::runtime_error("Empty parameter entry");
    values.push_back(std::stof(token));
  }
  return values;
}

void Save(const fs::path& file, const std::array<Result, 5>& outputs) {
  std::ofstream csv(file);
  Require(static_cast<bool>(csv), "Failed to write waveform CSV");
  csv << "sample,unchanged,instant,step24,reload_sample,kernel_sample\n";
  csv << std::setprecision(10);
  for (int i = 0; i < kOutput; ++i) {
    csv << i;
    for (const auto& r : outputs) csv << ',' << r.samples.at(i);
    csv << '\n';
  }
}

int main(int argc, char** argv) {
  try {
    Require(argc == 4,
            "Usage: real_pnam_kernel_probe <model.pnam> <output-dir> <params-csv>");
    std::ifstream in(argv[1]);
    Require(static_cast<bool>(in), "Cannot open model file");
    const Json model = Json::parse(in);
    Require(model.at("architecture") == "HyperWaveNet",
            "This probe expects a direct HyperWaveNet, not a container");
    const auto params = ParseParams(argv[3]);
    const auto& specs = model.at("config").at("params");
    Require(params.size() == specs.size(), "Wrong number of controls");
    auto validation = nam::get_dsp(model);
    auto* hyper = dynamic_cast<nam::wavenet::HyperWaveNet*>(validation.get());
    Require(hyper != nullptr, "Not a HyperWaveNet instance");
    hyper->SetParams(params);
    fs::create_directories(argv[2]);
    std::ofstream metrics(fs::path(argv[2]) / "metrics.csv");
    Require(static_cast<bool>(metrics), "Cannot create metrics.csv");
    metrics << "signal,automation,mode,render_ms,max_block_ms,rms_vs_reload,peak_vs_reload\n";
    std::cout << std::setprecision(12);

    const std::array<int, 5> modes = {-1, 0, 24, 1, -2};
    for (bool tone : {false, true}) for (bool automated : {false, true}) {
      const auto input = MakeInput(tone);
      std::array<Result, 5> results;
      for (size_t i = 0; i < modes.size(); ++i)
        results[i] = Run(model, params, input, modes[i], automated);
      const auto& reference = results[3].samples;
      for (size_t i = 0; i < modes.size(); ++i) {
        double sum = 0., peak = 0.;
        for (int sample = 0; sample < kOutput; ++sample) {
          const double diff = static_cast<double>(results[i].samples[sample])
                              - reference[sample];
          sum += diff * diff;
          peak = std::max(peak, std::abs(diff));
        }
        const double rms = std::sqrt(sum / kOutput);
        const char* sig = tone ? "tone" : "silence";
        const char* automation = automated ? "retarget_20ms" : "single";
        metrics << sig << ',' << automation << ',' << ModeName(modes[i])
                << ',' << results[i].elapsedMs << ',' << results[i].maxBlockMs
                << ',' << rms << ',' << peak << '\n';
        std::cout << sig << " " << automation << " " << ModeName(modes[i])
                  << " render_ms=" << results[i].elapsedMs
                  << " max_block_ms=" << results[i].maxBlockMs
                  << " peak_vs_reload=" << peak << " rms_vs_reload=" << rms << '\n';
        if (modes[i] == -2)
          Require(peak < 0.002, "Kernel path deviates from per-sample reload");
      }
      Save(fs::path(argv[2]) /
           ((tone ? "tone" : "silence") + std::string("_") +
            (automated ? "retarget_20ms" : "single") + "_waveforms.csv"), results);
    }
    std::cout << "REAL_PNAM_KERNEL_PROBE_PASSED\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "REAL_PNAM_KERNEL_PROBE_FAILED: " << e.what() << '\n';
    return 1;
  }
}
