cmake_minimum_required(VERSION 3.22)

project(NamParametricPlugin VERSION 0.5.1)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()

if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(external/JUCE)

set(TARGET_NAME NamParametricPlugin)

set(NAM_PLUGIN_FORMATS VST3 Standalone)
if(APPLE)
  list(APPEND NAM_PLUGIN_FORMATS AU)
endif()

set(AUDIODSPTOOLS_DIR ${CMAKE_CURRENT_LIST_DIR}/external/AudioDSPTools)
set(NAM_CORE_DIR ${CMAKE_CURRENT_LIST_DIR}/external/nam_core_parametric)
set(NAM_CORE_NAM_DIR ${NAM_CORE_DIR}/NAM)
set(NAM_CORE_EIGEN_DIR ${NAM_CORE_DIR}/Dependencies/eigen)
set(NAM_CORE_NLOHMANN_DIR ${NAM_CORE_DIR}/Dependencies/nlohmann)

if(NOT EXISTS ${AUDIODSPTOOLS_DIR}/dsp/ResamplingContainer/ResamplingContainer.h)
  message(FATAL_ERROR
    "Missing AudioDSPTools dependency at ${AUDIODSPTOOLS_DIR}. "
    "Run: git submodule update --init --recursive")
endif()

if(NOT EXISTS ${NAM_CORE_EIGEN_DIR}/Eigen/Core)
  message(FATAL_ERROR
    "Missing Eigen dependency at ${NAM_CORE_EIGEN_DIR}. "
    "Run: git submodule update --init --recursive")
endif()

file(GLOB NAM_CORE_SOURCES CONFIGURE_DEPENDS
  ${NAM_CORE_NAM_DIR}/*.cpp
  ${NAM_CORE_NAM_DIR}/*/*.cpp
)

add_library(nam_core_runtime OBJECT
  ${NAM_CORE_SOURCES}
)

set_target_properties(nam_core_runtime PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(nam_core_runtime
  SYSTEM PRIVATE
    ${NAM_CORE_DIR}
    ${NAM_CORE_NAM_DIR}
    ${NAM_CORE_EIGEN_DIR}
    ${NAM_CORE_NLOHMANN_DIR}
)

target_compile_definitions(nam_core_runtime PRIVATE
  EIGEN_MPL2_ONLY=1
  EIGEN_NO_DEBUG=1
  NAM_SAMPLE_FLOAT=1
  NAM_ENABLE_A2_FAST=1
)

if(MSVC)
  target_compile_options(nam_core_runtime PRIVATE /W0)
else()
  target_compile_options(nam_core_runtime PRIVATE -w)
endif()

add_library(audio_dsp_tools_runtime OBJECT
  ${AUDIODSPTOOLS_DIR}/dsp/dsp.cpp
  ${AUDIODSPTOOLS_DIR}/dsp/wav.cpp
  ${AUDIODSPTOOLS_DIR}/dsp/ImpulseResponse.cpp
)

set_target_properties(audio_dsp_tools_runtime PROPERTIES
  POSITION_INDEPENDENT_CODE ON
)

target_include_directories(audio_dsp_tools_runtime
  SYSTEM PRIVATE
    ${AUDIODSPTOOLS_DIR}
    ${AUDIODSPTOOLS_DIR}/dsp
    ${NAM_CORE_EIGEN_DIR}
)

target_compile_definitions(audio_dsp_tools_runtime PRIVATE
  EIGEN_MPL2_ONLY=1
  EIGEN_NO_DEBUG=1
)

if(MSVC)
  target_compile_options(audio_dsp_tools_runtime PRIVATE /W0)
else()
  target_compile_options(audio_dsp_tools_runtime PRIVATE -w)
endif()

juce_add_plugin(${TARGET_NAME}
  COMPANY_NAME "phillipmself"
  PRODUCT_NAME "NAM Parametric Plugin"
  ICON_BIG "${CMAKE_CURRENT_LIST_DIR}/Resources/TTSLogo.png"
  ICON_SMALL "${CMAKE_CURRENT_LIST_DIR}/Resources/TTSLogo.png"
  PLUGIN_MANUFACTURER_CODE PmSf
  PLUGIN_CODE NpPg
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  IS_MIDI_EFFECT FALSE
  FORMATS ${NAM_PLUGIN_FORMATS}
  COPY_PLUGIN_AFTER_BUILD FALSE
)

juce_generate_juce_header(${TARGET_NAME})
juce_add_binary_data(TtsBranding HEADER_NAME TtsBranding.h NAMESPACE TtsBranding
  SOURCES Resources/TTSLogo.png)

if(WIN32)
  # JUCE disables ASIO by default. Propagate this to the standalone wrapper too.
  target_compile_definitions(${TARGET_NAME} PUBLIC JUCE_ASIO=1)
endif()

target_sources(${TARGET_NAME} PRIVATE
  Source/DSP/IrEngine.cpp
  Source/DSP/IrEngine.h
  Source/DSP/NamModelEngine.cpp
  Source/DSP/NamModelEngine.h
  Source/NamParametricPluginEditor.cpp
  Source/NamParametricPluginEditor.h
  Source/NamParametricPluginProcessor.cpp
  Source/NamParametricPluginProcessor.h
  Source/UI/AboutOverlayComponent.cpp
  Source/UI/AboutOverlayComponent.h
  Source/UI/CalibrationSettingsComponent.cpp
  Source/UI/CalibrationSettingsComponent.h
  Source/UI/IrBarComponent.cpp
  Source/UI/IrBarComponent.h
  Source/UI/ModelBarComponent.cpp
  Source/UI/ModelBarComponent.h
  Source/UI/ModelParametersPanel.cpp
  Source/UI/ModelParametersPanel.h
  Source/UI/NamColours.h
  Source/UI/NamLookAndFeel.cpp
  Source/UI/NamLookAndFeel.h
  Source/UI/SegmentedSwitch.cpp
  Source/UI/SegmentedSwitch.h
  Source/UI/TopBarComponent.cpp
  Source/UI/TopBarComponent.h
)

# Important: put NAM object files directly into final wrapper binaries,
# not only the JUCE shared static archive, so static factory registrars
# are not dropped by archive dead-stripping.
foreach(wrapper_target IN ITEMS ${TARGET_NAME}_VST3 ${TARGET_NAME}_AU ${TARGET_NAME}_Standalone)
  if(TARGET ${wrapper_target})
    target_sources(${wrapper_target} PRIVATE
      $<TARGET_OBJECTS:nam_core_runtime>
    )

    if(APPLE)
      add_custom_command(TARGET ${wrapper_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
          "$<TARGET_BUNDLE_CONTENT_DIR:${wrapper_target}>/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_CURRENT_LIST_DIR}/THIRD-PARTY-NOTICES.md"
          "$<TARGET_BUNDLE_CONTENT_DIR:${wrapper_target}>/Resources/THIRD-PARTY-NOTICES.md"
        COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_CURRENT_LIST_DIR}/CREDITS.md"
          "$<TARGET_BUNDLE_CONTENT_DIR:${wrapper_target}>/Resources/CREDITS.md"
      )
    else()
      add_custom_command(TARGET ${wrapper_target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_CURRENT_LIST_DIR}/THIRD-PARTY-NOTICES.md"
          "$<TARGET_FILE_DIR:${wrapper_target}>/THIRD-PARTY-NOTICES.md"
        COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_CURRENT_LIST_DIR}/CREDITS.md"
          "$<TARGET_FILE_DIR:${wrapper_target}>/CREDITS.md"
      )
    endif()
  endif()
endforeach()

target_include_directories(${TARGET_NAME} PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/Source
)

target_include_directories(${TARGET_NAME} SYSTEM PRIVATE
  ${AUDIODSPTOOLS_DIR}
  ${NAM_CORE_DIR}
  ${NAM_CORE_NAM_DIR}
  ${NAM_CORE_EIGEN_DIR}
  ${NAM_CORE_NLOHMANN_DIR}
)

target_compile_definitions(${TARGET_NAME} PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  EIGEN_MPL2_ONLY=1
  EIGEN_NO_DEBUG=1
  NAM_SAMPLE_FLOAT=1
  NAM_ENABLE_A2_FAST=1
)

target_link_libraries(${TARGET_NAME}
  PRIVATE
    TtsBranding
    audio_dsp_tools_runtime
    juce::juce_audio_utils
    juce::juce_dsp
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
)

option(NAM_PARAMETRIC_BUILD_TESTS "Build NAM model smoke tests" ON)
if(NAM_PARAMETRIC_BUILD_TESTS)
  add_executable(nam_model_smoke_test
    Tests/NamModelEngineSmokeTest.cpp
    Source/DSP/NamModelEngine.cpp
    Source/DSP/NamModelEngine.h
    $<TARGET_OBJECTS:nam_core_runtime>
  )

  target_include_directories(nam_model_smoke_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/Source
  )

  target_include_directories(nam_model_smoke_test SYSTEM PRIVATE
    ${AUDIODSPTOOLS_DIR}
    ${NAM_CORE_DIR}
    ${NAM_CORE_NAM_DIR}
    ${NAM_CORE_EIGEN_DIR}
    ${NAM_CORE_NLOHMANN_DIR}
  )

  target_compile_definitions(nam_model_smoke_test PRIVATE
    EIGEN_MPL2_ONLY=1
    EIGEN_NO_DEBUG=1
    NAM_SAMPLE_FLOAT=1
    NAM_ENABLE_A2_FAST=1
  )

  add_executable(ir_normalization_test
    Tests/IrNormalizationTest.cpp
    Source/DSP/IrEngine.cpp
    $<TARGET_OBJECTS:audio_dsp_tools_runtime>)
  target_include_directories(ir_normalization_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/Source)
  target_include_directories(ir_normalization_test SYSTEM PRIVATE
    ${AUDIODSPTOOLS_DIR}
    ${NAM_CORE_EIGEN_DIR})
  if(BUILD_TESTING)
    add_test(NAME ir_normalization COMMAND ir_normalization_test)
    set_tests_properties(ir_normalization PROPERTIES TIMEOUT 60)
  endif()

  set(NAM_CONCAT_TEST_MODEL "" CACHE FILEPATH "ConcatWaveNet model used by CTest")
  set(NAM_HYPER_A2_TEST_DIR "${CMAKE_CURRENT_LIST_DIR}/Tests/fixtures" CACHE PATH "Directory generated by Tests/generate_hyper_a2_fixture.py")
  add_executable(hyper_a2_engine_test Tests/HyperA2EngineTest.cpp
    Source/DSP/NamModelEngine.cpp $<TARGET_OBJECTS:nam_core_runtime>)
  target_include_directories(hyper_a2_engine_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/Source)
  target_include_directories(hyper_a2_engine_test SYSTEM PRIVATE ${AUDIODSPTOOLS_DIR}
    ${NAM_CORE_DIR} ${NAM_CORE_NAM_DIR} ${NAM_CORE_EIGEN_DIR} ${NAM_CORE_NLOHMANN_DIR})
  target_compile_definitions(hyper_a2_engine_test PRIVATE EIGEN_MPL2_ONLY=1
    EIGEN_NO_DEBUG=1 NAM_SAMPLE_FLOAT=1 NAM_ENABLE_A2_FAST=1)
  juce_add_console_app(hyper_a2_processor_test PRODUCT_NAME "HyperA2 Processor Test")
  target_sources(hyper_a2_processor_test PRIVATE Tests/HyperA2ProcessorTest.cpp
    Tests/SwitchCombinationTest.cpp
    $<TARGET_OBJECTS:nam_core_runtime>)
  target_include_directories(hyper_a2_processor_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/Source)
  target_include_directories(hyper_a2_processor_test SYSTEM PRIVATE ${AUDIODSPTOOLS_DIR} ${NAM_CORE_DIR}
    ${NAM_CORE_NAM_DIR} ${NAM_CORE_EIGEN_DIR} ${NAM_CORE_NLOHMANN_DIR})
  target_compile_definitions(hyper_a2_processor_test PRIVATE EIGEN_MPL2_ONLY=1
    EIGEN_NO_DEBUG=1 NAM_SAMPLE_FLOAT=1 NAM_ENABLE_A2_FAST=1 JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
  target_link_libraries(hyper_a2_processor_test PRIVATE ${TARGET_NAME} juce::juce_audio_utils juce::juce_dsp)
  juce_add_console_app(static_export_test PRODUCT_NAME "Static NAM Export Test")
  target_sources(static_export_test PRIVATE Tests/StaticExportTest.cpp
    Tests/ControlTransitionTest.cpp
    $<TARGET_OBJECTS:nam_core_runtime>)
  target_include_directories(static_export_test PRIVATE ${CMAKE_CURRENT_LIST_DIR}/Source)
  target_include_directories(static_export_test SYSTEM PRIVATE ${AUDIODSPTOOLS_DIR} ${NAM_CORE_DIR}
    ${NAM_CORE_NAM_DIR} ${NAM_CORE_EIGEN_DIR} ${NAM_CORE_NLOHMANN_DIR})
  target_compile_definitions(static_export_test PRIVATE EIGEN_MPL2_ONLY=1
    EIGEN_NO_DEBUG=1 NAM_SAMPLE_FLOAT=1 NAM_ENABLE_A2_FAST=1 JUCE_MODAL_LOOPS_PERMITTED=1
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
  target_link_libraries(static_export_test PRIVATE ${TARGET_NAME} juce::juce_audio_utils juce::juce_dsp)
  if(BUILD_TESTING)
    add_test(NAME static_export COMMAND static_export_test "${CMAKE_CURRENT_BINARY_DIR}/static-export-results")
    set_tests_properties(static_export PROPERTIES TIMEOUT 180)
  endif()
  if(BUILD_TESTING AND EXISTS "${NAM_HYPER_A2_TEST_DIR}/manifest.json")
    add_test(NAME hyper_a2_engine COMMAND hyper_a2_engine_test "${NAM_HYPER_A2_TEST_DIR}")
    add_test(NAME hyper_a2_processor COMMAND hyper_a2_processor_test "${NAM_HYPER_A2_TEST_DIR}")
    set_tests_properties(hyper_a2_processor PROPERTIES TIMEOUT 180)
  endif()
  if(BUILD_TESTING AND EXISTS "${NAM_HYPER_A2_TEST_DIR}/switch_combinations.json")
    add_test(NAME switch_combinations COMMAND hyper_a2_processor_test
      --switches "${NAM_HYPER_A2_TEST_DIR}")
    set_tests_properties(switch_combinations PROPERTIES TIMEOUT 180)
  endif()
  set(NAM_HYPER_TEST_MODEL "" CACHE FILEPATH "HyperWaveNet model used by CTest")
  if(BUILD_TESTING AND EXISTS "${NAM_CONCAT_TEST_MODEL}" AND EXISTS "${NAM_HYPER_TEST_MODEL}")
    add_test(NAME nam_model_smoke
      COMMAND nam_model_smoke_test "${NAM_CONCAT_TEST_MODEL}" "${NAM_HYPER_TEST_MODEL}"
    )
  endif()
endif()
