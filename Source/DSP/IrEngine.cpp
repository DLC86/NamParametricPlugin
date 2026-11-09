#include "DSP/IrEngine.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>
#include <vector>

#include "dsp/Resample.h"

namespace namparametric::dsp {
namespace {

// Match paranam/ir.py::prepare_ir (64 logarithmically spaced frequencies from
// 100 to 5000 Hz, -48/+24 dB correction). AudioDSPTools applies an additional
// -18 dB * 48000 / sampleRate gain to its convolution weights. Undo that gain
// after processing, so an impulse with unit broadband response stays at 0 dB.
// Calculate from exactly the samples used by AudioDSPTools (cubic resampling,
// first 8192 taps) so the result is stable for long and resampled WAV files.
double MatchCompareIrGain(const ::dsp::ImpulseResponse::IRData& ir, double sampleRate) {
  if (!std::isfinite(sampleRate) || sampleRate <= 0.0 ||
      !std::isfinite(ir.mRawAudioSampleRate) || ir.mRawAudioSampleRate <= 0.0 ||
      ir.mRawAudio.empty())
    throw std::runtime_error("Invalid IR samples or sample rate");

  std::vector<float> resampled;
  if (ir.mRawAudioSampleRate == sampleRate) {
    resampled = ir.mRawAudio;
  } else {
    std::vector<float> padded(ir.mRawAudio.size() + 2, 0.0f);
    std::copy(ir.mRawAudio.begin(), ir.mRawAudio.end(), padded.begin() + 1);
    ::dsp::ResampleCubic<float>(
        padded, ir.mRawAudioSampleRate, sampleRate, 0.0, resampled);
  }
  resampled.resize(std::min<size_t>(resampled.size(), 8192));
  if (resampled.empty())
    throw std::runtime_error("The impulse response is empty");

  constexpr int kPoints = 64;
  constexpr double kPi = 3.1415926535897932384626433832795;
  const double top = std::min(5000.0, sampleRate * 0.49);
  if (top < 100.0)
    throw std::runtime_error("IR sample rate is too low");
  double power = 0.0;
  for (int i = 0; i < kPoints; ++i) {
    const double frequency =
        100.0 * std::pow(top / 100.0, static_cast<double>(i) / (kPoints - 1));
    const std::complex<double> step =
        std::polar(1.0, -2.0 * kPi * frequency / sampleRate);
    std::complex<double> phase(1.0, 0.0);
    std::complex<double> response(0.0, 0.0);
    for (const float tap : resampled) {
      response += static_cast<double>(tap) * phase;
      phase *= step;
    }
    power += std::norm(response);
  }
  power /= kPoints;
  // Compare also rejects silent IRs; avoid turning a noiseless or extremely
  // weak response into an unexpectedly loud output.
  if (!std::isfinite(power) || power <= 0.0)
    throw std::runtime_error("IR contains silence or invalid frequency response");
  const double correctionDb =
      std::clamp(-10.0 * std::log10(power), -48.0, 24.0);
  const double nativeGain = std::pow(10.0, -18.0 / 20.0) * 48000.0 / sampleRate;
  return std::pow(10.0, correctionDb / 20.0) / nativeGain;
}

}  // namespace

bool IrEngine::LoadIr(const std::string& irPath, const double sampleRate,
                      std::string& errorMessage) {
  errorMessage.clear();

  try {
    auto ir = std::make_unique<::dsp::ImpulseResponse>(irPath.c_str(), sampleRate);
    if (ir->GetWavState() != ::dsp::wav::LoadReturnCode::SUCCESS) {
      errorMessage = ::dsp::wav::GetMsgForLoadReturnCode(ir->GetWavState());
      return false;
    }

    const double gain = MatchCompareIrGain(ir->GetData(), sampleRate);
    mIr = std::move(ir);
    mNormalizationGain = gain;
    mSampleRate = sampleRate;
    return true;
  } catch (const std::exception& error) {
    errorMessage = error.what();
    return false;
  }
}

void IrEngine::Reset(const double sampleRate, const int maxBlockSize) {
  if (mIr == nullptr) {
    return;
  }

  constexpr double kSampleRateEpsilon = 0.5;
  if (std::abs(sampleRate - mSampleRate) > kSampleRateEpsilon) {
    const auto data = mIr->GetData();
    const double gain = MatchCompareIrGain(data, sampleRate);
    auto resampledIr = std::make_unique<::dsp::ImpulseResponse>(data, sampleRate);
    mIr = std::move(resampledIr);
    mNormalizationGain = gain;
    mSampleRate = sampleRate;
  }

  // Warm up the internal buffer allocation now, off the audio thread.
  const size_t warmupSize = static_cast<size_t>(std::max(maxBlockSize, 1));
  std::vector<float> warmupInput(warmupSize, 0.0f);
  std::vector<float> warmupOutput(warmupSize, 0.0f);
  Process(warmupInput.data(), warmupOutput.data(), static_cast<int>(warmupSize));
}

void IrEngine::Process(const float* input, float* output, const int numSamples) {
  if (numSamples <= 0) {
    return;
  }

  if (mIr == nullptr) {
    std::copy_n(input, numSamples, output);
    return;
  }

  mInputConversionScratch.resize(static_cast<size_t>(numSamples));
  std::copy_n(input, numSamples, mInputConversionScratch.begin());

  double* inputPtrs[1] = {mInputConversionScratch.data()};
  double** processed = mIr->Process(inputPtrs, 1, static_cast<size_t>(numSamples));
  std::transform(processed[0], processed[0] + numSamples, output,
                 [gain = mNormalizationGain](double value) {
                   return static_cast<float>(value * gain);
                 });
}

}  // namespace namparametric::dsp
