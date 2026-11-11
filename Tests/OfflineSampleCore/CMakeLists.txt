cmake_minimum_required(VERSION 3.22)
project(PNAMOfflineWeightExperiment LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Standalone core-only; all test executables share the same precompiled NAM core.
set(CORE "${CMAKE_CURRENT_LIST_DIR}/../../external/nam_core_parametric")
file(GLOB CORE_SOURCES CONFIGURE_DEPENDS
  "${CORE}/NAM/*.cpp"
  "${CORE}/NAM/*/*.cpp")
add_library(pnam_offline_core OBJECT ${CORE_SOURCES})
add_executable(offline_weight_smoothing_experiment
  "${CMAKE_CURRENT_LIST_DIR}/../OfflineSampleWeightSmoothingExperiment.cpp"
  $<TARGET_OBJECTS:pnam_offline_core>)
add_executable(real_pnam_kernel_probe
  "${CMAKE_CURRENT_LIST_DIR}/../RealPnamKernelProbe.cpp"
  $<TARGET_OBJECTS:pnam_offline_core>)

foreach(target pnam_offline_core offline_weight_smoothing_experiment real_pnam_kernel_probe)
  target_include_directories(${target} SYSTEM PRIVATE
    "${CORE}" "${CORE}/NAM"
    "${CORE}/Dependencies/eigen" "${CORE}/Dependencies/nlohmann")
  target_compile_definitions(${target} PRIVATE
    EIGEN_MPL2_ONLY=1 EIGEN_NO_DEBUG=1 NAM_SAMPLE_FLOAT=1 NAM_ENABLE_A2_FAST=1)
  if(MSVC)
    target_compile_options(${target} PRIVATE /O2)
  else()
    target_compile_options(${target} PRIVATE -O3 -w)
  endif()
endforeach()
