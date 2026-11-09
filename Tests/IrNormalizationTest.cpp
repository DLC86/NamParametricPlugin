// Regression for IR gain matching the 100-5000 Hz, 64-point normalization in
// ParaNAM Trainer's Compare page. These fixtures are generated locally; no
// external IR download or audio hardware is needed.
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "DSP/IrEngine.h"

namespace {
void Check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void Put16(std::ofstream& file, std::uint16_t value) {
  for (int i = 0; i < 2; ++i) file.put(static_cast<char>((value >> (8 * i)) & 255));
}

void Put32(std::ofstream& file, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) file.put(static_cast<char>((value >> (8 * i)) & 255));
}

// Signed 16-bit PCM accepted by the AudioDSPTools reader on all platforms.
void WriteIr(const std::filesystem::path& path, int sampleRate,
             const std::vector<float>& left,
             const std::vector<float>& right = {}) {
  const int channels = right.empty() ? 1 : 2;
  Check(channels == 1 || right.size() == left.size(), "Invalid stereo test IR");
  const std::uint32_t bytes = static_cast<std::uint32_t>(left.size() * channels * 2);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  Check(file.good(), "Could not write test WAV");
  file.write("RIFF", 4);
  Put32(file, 36 + bytes);
  file.write("WAVEfmt ", 8);
  Put32(file, 16);
  Put16(file, 1);
  Put16(file, static_cast<std::uint16_t>(channels));
  Put32(file, static_cast<std::uint32_t>(sampleRate));
  Put32(file, static_cast<std::uint32_t>(sampleRate * channels * 2));
  Put16(file, static_cast<std::uint16_t>(channels * 2));
  Put16(file, 16);
  file.write("data", 4);
  Put32(file, bytes);
  const auto encode = [&](float sample) {
    Check(std::isfinite(sample) && std::abs(sample) <= 1.0f, "Bad test sample");
    const auto pcm = static_cast<std::int16_t>(std::lround(sample * 32767));
    Put16(file, static_cast<std::uint16_t>(pcm));
  };
  for (size_t i = 0; i < left.size(); ++i) {
    encode(left[i]);
    if (channels == 2) encode(right[i]);
  }
  Check(file.good(), "Could not finish test WAV");
}

std::vector<float> RenderImpulse(namparametric::dsp::IrEngine& engine,
                                 int numSamples = 512) {
  std::vector<float> input(numSamples, 0.0f), output(numSamples, 0.0f);
  input[0] = 1.0f;
  // Exercise variable audio callback sizes; never reinitialize the IR between
  // blocks, and make sure its calibration is the same for both sizes.
  for (int offset = 0; offset < numSamples; offset += 31) {
    const int count = std::min(31, numSamples - offset);
    engine.Process(input.data() + offset, output.data() + offset, count);
  }
  return output;
}

double MeanFrequencyResponsePower(const std::vector<float>& impulse,
                                  double sampleRate) {
  constexpr double pi = 3.14159265358979323846;
  double total = 0.0;
  for (int i = 0; i < 64; ++i) {
    const double f = 100.0 * std::pow(
        std::min(5000.0, sampleRate * 0.49) / 100.0, i / 63.0);
    std::complex<double> phase(1.0, 0.0), response(0.0, 0.0);
    const auto step = std::polar(1.0, -2.0 * pi * f / sampleRate);
    for (float sample : impulse) {
      response += static_cast<double>(sample) * phase;
      phase *= step;
    }
    total += std::norm(response);
  }
  return total / 64.0;
}

void TestUnitResponse(const std::filesystem::path& directory, int rate,
                      float amplitude, bool stereo) {
  const auto path = directory / "unity-ir.wav";
  WriteIr(path, rate, {amplitude}, stereo ? std::vector<float>{0.1f} :
                                                std::vector<float>{});
  namparametric::dsp::IrEngine engine;
  std::string error;
  Check(engine.LoadIr(path.string(), rate, error), error);
  engine.Reset(rate, 128);
  const auto output = RenderImpulse(engine);
  Check(std::abs(output[0] - 1.0f) < 0.002f,
        "An impulse must not lose approximately 18 dB when IR is enabled");
  Check(MeanFrequencyResponsePower(output, rate) > 0.995 &&
            MeanFrequencyResponsePower(output, rate) < 1.005,
        "IR broadband magnitude should be unity, as in Trainer Compare");
  Check(std::abs(output[1]) < 0.0001f, "Impulse leaked into the next sample");
}

void TestFrequencyNormalization(const std::filesystem::path& directory) {
  const auto path = directory / "cab-ir.wav";
  // A mild low-pass response proves the normalization is spectral, not based
  // on time-domain peak, sum or the AudioDSPTools fixed -18dB gain.
  WriteIr(path, 48000, {0.2f, 0.6f, 0.2f});
  namparametric::dsp::IrEngine engine;
  std::string error;
  Check(engine.LoadIr(path.string(), 48000, error), error);
  engine.Reset(48000, 64);
  auto output = RenderImpulse(engine);
  Check(std::abs(10.0 * std::log10(MeanFrequencyResponsePower(output, 48000))) <
            0.03,
        "IR band power must match Compare's 0 dB reference");
  // Changing the host rate regenerates both the native convolution weights
  // and the compensation; a stale -18dB / rate correction would fail here.
  engine.Reset(96000, 64);
  output = RenderImpulse(engine);
  Check(std::abs(10.0 * std::log10(MeanFrequencyResponsePower(output, 96000))) <
            0.06,
        "Resampling must preserve the corrected IR band power");
}

void TestInvalidIr(const std::filesystem::path& directory) {
  const auto path = directory / "silent-ir.wav";
  WriteIr(path, 48000, {0, 0, 0});
  namparametric::dsp::IrEngine engine;
  std::string error;
  Check(!engine.LoadIr(path.string(), 48000, error) && !error.empty(),
        "Silent IR must produce a readable load failure");
}
}  // namespace

int main() {
  try {
    const auto directory = std::filesystem::temp_directory_path() /
                           "nam-parametric-ir-regression";
    std::filesystem::create_directories(directory);
    for (int rate : {44100, 48000, 96000}) {
      TestUnitResponse(directory, rate, 0.5f, false);
      TestUnitResponse(directory, rate, 0.25f, true);
    }
    TestFrequencyNormalization(directory);
    TestInvalidIr(directory);
    std::cout << "IR normalization matches Compare and has unity band gain\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
