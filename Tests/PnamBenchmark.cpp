#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "DSP/NamModelEngine.h"
#include "json.hpp"

int main(int argc, char** argv) {
  using namespace namparametric::dsp;
  try {
    if (argc != 2) throw std::runtime_error("Usage: pnam_benchmark <model>");
    nlohmann::json results = nlohmann::json::array();
    for (auto size : {ModelSize::Full, ModelSize::Lite}) {
      for (int block : {32, 64, 128, 256}) {
        for (bool moving : {false, true}) {
          NamModelEngine engine;
          std::string error;
          if (!engine.LoadModel(argv[1], error, size)) throw std::runtime_error(error);
          engine.SetControlTransition(ControlTransition::Eco);
          engine.Reset(48000, block);
          std::vector<float> input(block), output(block);
          auto params = engine.GetParameterValues();
          const auto& specs = engine.GetParameterInfos();
          std::vector<double> times;
          double checksum = 0;
          for (int iteration = 0; iteration < 1200; ++iteration) {
            for (int i = 0; i < block; ++i)
              input[i] = 0.1f * std::sin(0.023f * static_cast<float>(iteration * block + i));
            const auto start = std::chrono::steady_clock::now();
            if (moving) {
              for (size_t p = 0; p < params.size(); ++p) {
                const auto& spec = specs[p];
                const float t = static_cast<float>((iteration * 7 + p * 31) % 101) / 100;
                params[p] = spec.IsSwitch() ? static_cast<float>(iteration % spec.enumNames.size())
                                            : spec.minValue + t * (spec.maxValue - spec.minValue);
              }
              if (!engine.SetParameterValues(params)) throw std::runtime_error("Invalid controls");
            }
            engine.Process(input.data(), output.data(), block);
            const auto end = std::chrono::steady_clock::now();
            if (iteration >= 200)
              times.push_back(std::chrono::duration<double, std::micro>(end - start).count());
            checksum += output.back();
          }
          std::sort(times.begin(), times.end());
          double mean = 0;
          for (auto t : times) mean += t / static_cast<double>(times.size());
          results.push_back({{"size", size == ModelSize::Full ? "full" : "lite"},
                             {"block", block},
                             {"moving", moving},
                             {"mean_us", mean},
                             {"p99_us", times[times.size() * 99 / 100]},
                             {"max_us", times.back()},
                             {"checksum", checksum}});
        }
      }
    }
    std::cout << results.dump(2) << '\n';
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
