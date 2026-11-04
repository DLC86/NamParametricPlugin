#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "get_dsp.h"
#include "wavenet/hyper_a2.h"
#include "wavenet/hyperwavenet.h"

int main(int argc, char** argv) {
  try {
    if (argc != 2) throw std::runtime_error("Usage: pnam_fast_path_test <fixtures>");
    double maximum = 0;
    for (const char* name : {"hyper_a2_full.nam", "hyper_a2_low_rank.nam"}) {
      std::ifstream file(std::filesystem::path(argv[1]) / name);
      const auto source = nlohmann::json::parse(file);
      for (bool lite : {false, true}) {
        auto child = nam::wavenet::SelectHyperA2Branch(source, lite);
        auto config = nam::wavenet::create_hyperwavenet_config(child.at("config"), 48000);
        auto* hyper = dynamic_cast<nam::wavenet::HyperWaveNetConfig*>(config.get());
        if (!hyper || !hyper->fast_audio_config)
          throw std::runtime_error("A2 fast path not selected");
        hyper->fast_audio_config.reset();
        auto generic = config->create(child.at("weights").get<std::vector<float>>(), 48000);
        auto fast = nam::get_dsp(child);
        auto* oldControls = dynamic_cast<nam::IParametricControl*>(generic.get());
        auto* controls = dynamic_cast<nam::IParametricControl*>(fast.get());
        for (int block : {1, 7, 31, 32, 64, 257, 1024}) {
          generic->Reset(48000, block);
          fast->Reset(48000, block);
          std::vector<float> input(block), oldOutput(block), output(block);
          std::vector<float> values(controls->GetParams().begin(), controls->GetParams().end());
          for (int iteration = 0; iteration < 200; ++iteration) {
            const auto& specs = controls->GetParamSpecs();
            for (size_t i = 0; i < specs.size(); ++i) {
              const auto& spec = specs[i];
              values[i] =
                  spec.type == "switch"
                      ? static_cast<float>(iteration % spec.enum_names.size())
                      : spec.min + (spec.max - spec.min) * static_cast<float>(iteration % 17) / 16;
            }
            oldControls->SetParams(values);
            controls->SetParams(values);
            for (int i = 0; i < block; ++i)
              input[i] = 0.1f * std::sin(0.031f * static_cast<float>(iteration * block + i));
            auto* in = input.data();
            auto* a = oldOutput.data();
            auto* b = output.data();
            generic->process(&in, &a, block);
            fast->process(&in, &b, block);
            for (int i = 0; i < block; ++i) {
              if (!std::isfinite(output[i])) throw std::runtime_error("Nonfinite optimized output");
              maximum = std::max(maximum, std::abs(static_cast<double>(output[i]) - oldOutput[i]));
            }
          }
        }
      }
    }
    if (maximum > 2e-5) throw std::runtime_error("Changing weights changed audio/history");
    std::cout << "Fast/generic automation parity max_error=" << maximum << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
