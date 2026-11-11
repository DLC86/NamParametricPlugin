// Offline-only experiment: one HyperWaveNet DSP instance per sequential run.
// Not linked into the plugin. Compare *actual* coefficient reloads every sample
// against 0.5/1/2/4 ms, plus instantaneous weights, on identical input.
// The hypernetwork is evaluated once per destination, not once per sample.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

#include "get_dsp.h"
#include "parametric_control.h"
#include "wavenet/hyperwavenet.h"

using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr int kSampleRate = 48000;
constexpr int kBlockSize = 257;
constexpr int kPrerollFrames = 16000;
constexpr int kTransitionFrames = 1920;  // 40 ms
constexpr int kMeasureFrames = 4800;    // 100 ms, enough to settle.
constexpr std::array<int, 9> kSteps = {-1, 0, 192, 96, 48, 24, 8, 1, -2};

void Require(bool ok, const std::string& error) {
  if (!ok) throw std::runtime_error(error);
}

Json ReadJson(const fs::path& path) {
  std::ifstream stream(path);
  Require(static_cast<bool>(stream), "Cannot open fixture: " + path.string());
  return Json::parse(stream);
}

Json SelectBranch(const Json& container, bool lite) {
  const auto& branch = container.at("config").at("submodels").at(lite ? 0 : 1);
  Require(branch.at("architecture") == "HyperWaveNet", "Not a HyperWaveNet branch");
  const size_t offset = branch.at("weight_offset").get<size_t>();
  const size_t count = branch.at("weight_count").get<size_t>();
  Require(offset + count <= container.at("weights").size(), "Invalid branch weight span");
  Json out = container;
  out["architecture"] = "HyperWaveNet";
  out["config"] = branch.at("config");
  out["weights"] = Json::array();
  for (size_t i = offset; i < offset + count; ++i)
    out["weights"].push_back(container["weights"][i]);
  return out;
}

std::string StepName(int frames) {
  if (frames == -2) return "kernel_sample_1";
  if (frames == -1) return "unchanged";
  if (frames == 0) return "instant";
  if (frames == 1) return "sample_1";
  return "step_" + std::to_string(frames);
}

struct Result {
  int step = -1;
  std::vector<float> output;
  double milliseconds = 0;
};

std::vector<float> MakeInput(bool tone) {
  std::vector<float> input(kPrerollFrames + kMeasureFrames, 0.f);
  if (tone) {
    for (size_t i = 0; i < input.size(); ++i) {
      const double t = static_cast<double>(i) / kSampleRate;
      input[i] = static_cast<float>(
          0.13 * std::sin(2 * std::numbers::pi * 440.0 * t) +
          0.07 * std::sin(2 * std::numbers::pi * 3197.0 * t));
    }
  }
  return input;
}

Result Run(const Json& source, int stepFrames, const std::vector<float>& input) {
  auto model = nam::get_dsp(source);
  auto* hyper = dynamic_cast<nam::wavenet::HyperWaveNet*>(model.get());
  Require(hyper != nullptr, "Fixture must instantiate HyperWaveNet");
  Require(model->NumInputChannels() == 1 && model->NumOutputChannels() == 1,
          "Only mono models are supported");
  // We deliberately use the existing tested WaveNet implementation, including
  // its preallocated one-sample DSP path. No audio-rate call to the hypernetwork.
  model->SetPrewarmOnReset(false);
  model->Reset(kSampleRate, kBlockSize);
  std::vector<float> preroll(kPrerollFrames);
  for (int offset = 0; offset < kPrerollFrames; offset += kBlockSize) {
    int count = std::min(kBlockSize, kPrerollFrames - offset);
    float* in = const_cast<float*>(input.data() + offset);
    float* out = preroll.data() + offset;
    model->process(&in, &out, count);
  }
  hyper->ConfigureWeightSmoothing(stepFrames == -2 || stepFrames > 0 ? kTransitionFrames : 0,
                                  stepFrames == -2 ? 0 : std::max(1, stepFrames));
  if (stepFrames != -1) {
    const std::vector<float> target{8.0f, 1.0f};
    dynamic_cast<nam::IParametricControl&>(*model).SetParams(target);
  }
  Result result;
  result.step = stepFrames;
  result.output.resize(kMeasureFrames);
  const auto start = std::chrono::steady_clock::now();
  for (int offset = 0; offset < kMeasureFrames; offset += kBlockSize) {
    const int count = std::min(kBlockSize, kMeasureFrames - offset);
    float* in = const_cast<float*>(input.data() + kPrerollFrames + offset);
    float* out = result.output.data() + offset;
    model->process(&in, &out, count);
  }
  result.milliseconds = std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - start).count();
  Require(std::all_of(result.output.begin(), result.output.end(),
                      [](float sample) { return std::isfinite(sample); }),
          "Non-finite audio in offline interpolation experiment");
  return result;
}

// Radix-2 Fourier transform for measuring the *actual full frequency band*,
// not sparse sample points. The signal is zero-padded; normalization below is
// to the unpadded duration so measurements remain comparable.
void FFT(std::vector<std::complex<double>>& values) {
  const size_t n = values.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(values[i], values[j]);
  }
  for (size_t width = 2; width <= n; width <<= 1) {
    const double angle = -2.0 * std::numbers::pi / static_cast<double>(width);
    const std::complex<double> root(std::cos(angle), std::sin(angle));
    for (size_t start = 0; start < n; start += width) {
      std::complex<double> w(1, 0);
      for (size_t i = 0; i < width / 2; ++i) {
        const auto u = values[start + i], v = values[start + i + width / 2] * w;
        values[start + i] = u + v;
        values[start + i + width / 2] = u - v;
        w *= root;
      }
    }
  }
}

double HighBandRms(const std::vector<double>& data, int lowHz = 2000, int highHz = 20000) {
  size_t size = 1;
  while (size < data.size()) size <<= 1;
  std::vector<std::complex<double>> spec(size);
  for (size_t i = 0; i < data.size(); ++i) spec[i] = data[i];
  FFT(spec);
  const size_t low = static_cast<size_t>(
      std::ceil(static_cast<double>(lowHz) * size / kSampleRate));
  const size_t high = static_cast<size_t>(
      std::floor(static_cast<double>(highHz) * size / kSampleRate));
  double energy = 0;
  for (size_t i = low; i <= high; ++i) energy += 2.0 * std::norm(spec[i]);
  return std::sqrt(energy / (static_cast<double>(size) * data.size()));
}

double Rms(const std::vector<double>& data) {
  double power = 0;
  for (auto x : data) power += x * x;
  return std::sqrt(power / std::max<size_t>(1, data.size()));
}

struct Metrics {
  double peakDerivative = 0;
  double rmsDerivative = 0;
  double highBandDerivative = 0;
  double rmsChange = 0;
  double residualToSample = 0;
  double residualPeak = 0;
  double endDifference = 0;
};
Metrics Measure(const Result& run, const Result& unchanged, const Result& perSample) {
  Require(run.output.size() == unchanged.output.size() &&
          run.output.size() == perSample.output.size(), "Unequal output lengths");
  std::vector<double> diff(run.output.size()), derivative(run.output.size());
  std::vector<double> residual(run.output.size());
  double prior = 0;
  Metrics m;
  for (size_t i = 0; i < run.output.size(); ++i) {
    diff[i] = static_cast<double>(run.output[i]) - unchanged.output[i];
    derivative[i] = diff[i] - prior;
    prior = diff[i];
    m.peakDerivative = std::max(m.peakDerivative, std::abs(derivative[i]));
    residual[i] = static_cast<double>(run.output[i]) - perSample.output[i];
    m.residualPeak = std::max(m.residualPeak, std::abs(residual[i]));
  }
  m.rmsDerivative = Rms(derivative);
  m.highBandDerivative = HighBandRms(derivative);
  m.rmsChange = Rms(diff);
  m.residualToSample = Rms(residual);
  m.endDifference = std::abs(run.output.back() - perSample.output.back());
  return m;
}

void WriteWaveforms(const fs::path& file, const std::vector<Result>& results) {
  std::ofstream out(file);
  Require(static_cast<bool>(out), "Cannot write " + file.string());
  out << "sample";
  for (const auto& r : results) out << "," << StepName(r.step);
  out << "\n" << std::setprecision(10);
  for (int i = 0; i < kMeasureFrames; ++i) {
    out << i;
    for (const auto& r : results) out << "," << r.output[i];
    out << "\n";
  }
}

int main(int argc, char* argv[]) {
  try {
    Require(argc == 3, "Usage: offline_weight_smoothing_experiment <fixture-folder> <output-folder>");
    const fs::path fixtureDir = argv[1], outputDir = argv[2];
    fs::create_directories(outputDir);
    std::ofstream metrics(outputDir / "metrics.csv");
    Require(static_cast<bool>(metrics), "Cannot write metrics.csv");
    metrics << std::setprecision(12)
            << "model,size,input,step_frames,step_ms,render_ms,peak_dy,"
               "rms_dy,high_band_2k_20k_dy,rms_control_change,"
               "rms_vs_samplewise,peak_vs_samplewise,end_difference\n";
    std::cout << std::setprecision(9);
    for (const char* flavor : {"low_rank", "full"}) {
      const Json container = ReadJson(fixtureDir / (std::string("hyper_a2_") + flavor + ".nam"));
      for (bool lite : {false, true}) {
        const Json branch = SelectBranch(container, lite);
        const auto& specs = branch.at("config").at("params");
        Require(specs.size() == 2 && specs[0].at("min") <= 8 &&
                    specs[0].at("max") >= 8 && specs[1].at("max") >= 1,
                "Fixture target outside param range");
        for (bool tone : {false, true}) {
          const auto input = MakeInput(tone);
          std::vector<Result> results;
          results.reserve(kSteps.size());
          for (int step : kSteps) results.emplace_back(Run(branch, step, input));
          const Result& unchanged = results.front();
          const Result& samplewise = results[results.size() - 2];
          const Result& kernelwise = results.back();
          double kernelMax = 0., kernelRms = 0.;
          for (size_t n = 0; n < samplewise.output.size(); ++n) {
            const double d = static_cast<double>(kernelwise.output[n]) - samplewise.output[n];
            kernelMax = std::max(kernelMax, std::abs(d));
            kernelRms += d * d;
          }
          kernelRms = std::sqrt(kernelRms / samplewise.output.size());
          std::cout << "KERNEL_vs_RELOAD model=" << flavor
                    << " size=" << (lite ? "lite" : "full")
                    << " input=" << (tone ? "tone" : "silence")
                    << " max=" << kernelMax << " rms=" << kernelRms << "\n";
          Require(kernelMax < 0.002, "Kernel per-sample output diverges from one-sample reload reference");
          const std::string stem = std::string(flavor) + (lite ? "_lite_" : "_full_") +
                                   (tone ? "tone" : "silence");
          WriteWaveforms(outputDir / (stem + "_waveforms.csv"), results);
          for (const auto& run : results) {
            const Metrics m = Measure(run, unchanged, samplewise);
            metrics << flavor << "," << (lite ? "lite" : "full") << ","
                    << (tone ? "tone" : "silence") << "," << run.step << ","
                    << (run.step <= 0 ? 0. : 1000. * run.step / kSampleRate) << ","
                    << run.milliseconds << "," << m.peakDerivative << ","
                    << m.rmsDerivative << "," << m.highBandDerivative << ","
                    << m.rmsChange << "," << m.residualToSample << ","
                    << m.residualPeak << "," << m.endDifference << "\n";
            std::cout << stem << " " << StepName(run.step)
                      << " render_ms=" << run.milliseconds
                      << " peak_dy=" << m.peakDerivative
                      << " hi_band_dy=" << m.highBandDerivative
                      << " residual_rms_vs_sample=" << m.residualToSample
                      << " final_diff=" << m.endDifference << "\n";
          }
        }
      }
    }
    std::cout << "OFFLINE_EXPERIMENT_COMPLETE, waveforms + metrics at "
              << outputDir.string() << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "OFFLINE_EXPERIMENT_FAILED: " << error.what() << "\n";
    return 1;
  }
}
