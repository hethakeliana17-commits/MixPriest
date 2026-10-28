cmake_minimum_required(VERSION 3.22)
project(StemMaster VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

include(FetchContent)

# ---- JUCE ------------------------------------------------------------
FetchContent_Declare(JUCE
  GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
  GIT_TAG 8.0.4
  GIT_SHALLOW TRUE)

# ---- ONNX Runtime (prebuilt CPU binaries) ------------------------------
set(ORT_VER 1.18.1)
set(ORT_BASE https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VER})
if(WIN32)
  set(ORT_URL ${ORT_BASE}/onnxruntime-win-x64-${ORT_VER}.zip)
elseif(APPLE)
  set(ORT_URL ${ORT_BASE}/onnxruntime-osx-universal2-${ORT_VER}.tgz)
else()
  set(ORT_URL ${ORT_BASE}/onnxruntime-linux-x64-${ORT_VER}.tgz)
endif()
FetchContent_Declare(onnxruntime URL ${ORT_URL})

FetchContent_MakeAvailable(JUCE onnxruntime)
set(ORT_DIR ${onnxruntime_SOURCE_DIR})

if(WIN32)
  set(ORT_LIB ${ORT_DIR}/lib/onnxruntime.lib)
elseif(APPLE)
  set(ORT_LIB ${ORT_DIR}/lib/libonnxruntime.dylib)
else()
  set(ORT_LIB ${ORT_DIR}/lib/libonnxruntime.so)
endif()

# ---- Plugin ------------------------------------------------------------
set(STEM_FORMATS Standalone VST3)
if(APPLE)
  list(APPEND STEM_FORMATS AU)
endif()

juce_add_plugin(StemMaster
  COMPANY_NAME "YourName"
  IS_SYNTH FALSE
  NEEDS_MIDI_INPUT FALSE
  NEEDS_MIDI_OUTPUT FALSE
  PLUGIN_MANUFACTURER_CODE Yrnm
  PLUGIN_CODE Stmm
  FORMATS ${STEM_FORMATS}
  PRODUCT_NAME "StemMaster"
  COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(StemMaster PRIVATE
  Source/PluginProcessor.cpp
  Source/PluginEditor.cpp
  Source/StemSeparator.cpp)

target_compile_definitions(StemMaster PUBLIC
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_VST3_CAN_REPLACE_VST2=0
  JUCE_USE_MP3AUDIOFORMAT=1
  JUCE_DISPLAY_SPLASH_SCREEN=0)

target_include_directories(StemMaster PRIVATE ${ORT_DIR}/include)

target_link_libraries(StemMaster
  PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_formats
    juce::juce_dsp
    ${ORT_LIB}
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags)

# Make the ONNX Runtime shared library findable at run time.
foreach(fmt ${STEM_FORMATS})
  if(TARGET StemMaster_${fmt})
    if(NOT WIN32)
      set_target_properties(StemMaster_${fmt} PROPERTIES BUILD_RPATH "${ORT_DIR}/lib")
    else()
      add_custom_command(TARGET StemMaster_${fmt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${ORT_DIR}/lib/onnxruntime.dll
                $<TARGET_FILE_DIR:StemMaster_${fmt}>)
    endif()
  endif()
endforeach()
