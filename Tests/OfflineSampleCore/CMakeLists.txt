cmake_minimum_required(VERSION 3.22)
project(PNAMOfflineWeightExperiment LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Standalone core-only: no JUCE, VST, AudioDSPTools or packaged plugin.
# The pinned HyperWaveNet weight patch is applied to the checkout before build.
set(CORE "${CMAKE_CURRENT_LIST_DIR}/../../external/nam_core_parametric")
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
  "${CORE}/NAM/*.cpp"
  "${CORE}/NAM/*/*.cpp")
add_executable(offline_weight_smoothing_experiment
  "${CMAKE_CURRENT_LIST_DIR}/../OfflineSampleWeightSmoothingExperiment.cpp"
  ${CORE_SOURCES})
target_include_directories(offline_weight_smoothing_experiment SYSTEM PRIVATE
  "${CORE}" "${CORE}/NAM"
  "${CORE}/Dependencies/eigen" "${CORE}/Dependencies/nlohmann")
target_compile_definitions(offline_weight_smoothing_experiment PRIVATE
  EIGEN_MPL2_ONLY=1 EIGEN_NO_DEBUG=1 NAM_SAMPLE_FLOAT=1 NAM_ENABLE_A2_FAST=1)
if(MSVC)
  target_compile_options(offline_weight_smoothing_experiment PRIVATE /O2)
else()
  target_compile_options(offline_weight_smoothing_experiment PRIVATE -O3 -w)
endif()
