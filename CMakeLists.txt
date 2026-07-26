cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION.txt" HARMONIZER_VERSION LIMIT_COUNT 1)
if(NOT HARMONIZER_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "VERSION.txt must contain a semantic version such as 1.2.3")
endif()

project(Harmonizer VERSION "${HARMONIZER_VERSION}" LANGUAGES C CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)
pkg_check_modules(AUBIO REQUIRED IMPORTED_TARGET aubio)

# Rubber Band 4 introduced the low-latency LiveShifter used by this project.
# Ubuntu 24.04 and MSYS2 still package Rubber Band 3, so build the official
# single-file implementation to keep setup identical across platforms.
FetchContent_Declare(
  rubberband
  URL https://github.com/breakfastquay/rubberband/archive/refs/tags/v4.0.0.tar.gz
  URL_HASH SHA256=24300f48a8014b7c863b573a9647e61b1b19b37875e2cdd92005e64c6424d266
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_GetProperties(rubberband)
if(NOT rubberband_POPULATED)
  FetchContent_Populate(rubberband)
endif()

add_library(rubberband_live STATIC
  "${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp"
)
target_include_directories(rubberband_live PUBLIC "${rubberband_SOURCE_DIR}")

add_executable(harmonizer_web
  harmonizer_web.cpp
  harmonizer_rubberband_engine.hpp
)
target_compile_options(harmonizer_web PRIVATE
  $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra>
)
target_link_libraries(harmonizer_web PRIVATE
  PkgConfig::PORTAUDIO
  PkgConfig::AUBIO
  rubberband_live
  Threads::Threads
)

if(APPLE)
  target_link_libraries(rubberband_live PUBLIC "-framework Accelerate")
elseif(WIN32)
  target_link_libraries(harmonizer_web PRIVATE ws2_32)
endif()

add_custom_command(TARGET harmonizer_web POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:harmonizer_web>/web"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
          "$<TARGET_FILE_DIR:harmonizer_web>/web/index.html"
)

option(HARMONIZER_BUILD_BACKEND_LAB
  "Build isolated pitch-shifter comparison executables" OFF)

if(HARMONIZER_BUILD_BACKEND_LAB)
  add_executable(output_bridge_controller_test
    tests/output_bridge_controller_test.cpp
    backends/output_bridge_controller.hpp
  )
  target_include_directories(output_bridge_controller_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_compile_options(output_bridge_controller_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra>
  )
  add_test(NAME output_bridge_controller COMMAND output_bridge_controller_test)

  add_executable(parallel_pitch_tracker_test
    tests/parallel_pitch_tracker_test.cpp
    backends/predictive_pitch_tracker.hpp
  )
  target_include_directories(parallel_pitch_tracker_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_compile_options(parallel_pitch_tracker_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra>
  )
  add_test(NAME parallel_pitch_tracker COMMAND parallel_pitch_tracker_test)

  add_executable(collier_effects_test
    tests/collier_effects_test.cpp
    collier_effects.hpp
  )
  target_include_directories(collier_effects_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
  )
  target_compile_options(collier_effects_test PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra>
  )
  add_test(NAME collier_effects COMMAND collier_effects_test)

  set(rubberband_live128_source "${CMAKE_BINARY_DIR}/rubberband-live128-source")
  file(REMOVE_RECURSE "${rubberband_live128_source}")
  file(COPY "${rubberband_SOURCE_DIR}/" DESTINATION "${rubberband_live128_source}")
  set(rubberband_live128_impl
      "${rubberband_live128_source}/src/finer/R3LiveShifter.cpp")
  file(READ "${rubberband_live128_impl}" rubberband_live128_contents)
  set(rubberband_live128_original
      "R3LiveShifter::getBlockSize() const\n{\n    return 512;\n}")
  set(rubberband_live128_replacement
      "R3LiveShifter::getBlockSize() const\n{\n    return 128;\n}")
  string(FIND "${rubberband_live128_contents}"
              "${rubberband_live128_original}"
              rubberband_live128_match)
  if(rubberband_live128_match EQUAL -1)
    message(FATAL_ERROR "Rubber Band LiveShifter block-size patch no longer applies")
  endif()
  string(REPLACE "${rubberband_live128_original}"
                 "${rubberband_live128_replacement}"
                 rubberband_live128_contents
                 "${rubberband_live128_contents}")
  file(WRITE "${rubberband_live128_impl}" "${rubberband_live128_contents}")
  add_library(rubberband_live128 STATIC
    "${rubberband_live128_source}/single/RubberBandSingle.cpp"
  )
  target_include_directories(rubberband_live128 PUBLIC "${rubberband_live128_source}")
  if(APPLE)
    target_link_libraries(rubberband_live128 PUBLIC "-framework Accelerate")
  endif()

  FetchContent_Declare(
    signalsmith_linear
    URL https://github.com/Signalsmith-Audio/linear/archive/refs/tags/0.3.1.tar.gz
    URL_HASH SHA256=b294471f1306baa4d968b230a8836924680e9ca068a667f4659259d66edbe9bc
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(signalsmith_linear)
  FetchContent_Declare(
    signalsmith_stretch
    URL https://github.com/Signalsmith-Audio/signalsmith-stretch/archive/57b93f4e9206a089a45387eaa39bdc9f310d3308.tar.gz
    URL_HASH SHA256=ad02e24334438b203e81d44f6c9906f3c6773e90a4ea923bb3e73d15697187d6
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  )
  FetchContent_MakeAvailable(signalsmith_stretch)

  function(add_harmonizer_backend target source)
    add_executable(${target} ${source})
    target_compile_options(${target} PRIVATE
      $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra>
    )
    target_link_libraries(${target} PRIVATE
      PkgConfig::PORTAUDIO
      PkgConfig::AUBIO
      Threads::Threads
      ${ARGN}
    )
    if(WIN32)
      target_link_libraries(${target} PRIVATE ws2_32)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/web"
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
              "$<TARGET_FILE_DIR:${target}>/web/index.html"
    )
  endfunction()

  add_harmonizer_backend(
    harmonizer_web_rubberband_live_reference
    backends/harmonizer_web_rubberband_live_reference.cpp
    rubberband_live
  )
  add_harmonizer_backend(
    harmonizer_web_parallel
    backends/harmonizer_web_parallel.cpp
    rubberband_live
  )
  add_harmonizer_backend(
    harmonizer_web_rubberband_r2
    backends/harmonizer_web_rubberband_r2.cpp
    rubberband_live
  )
  add_harmonizer_backend(
    harmonizer_web_rubberband_live128
    backends/harmonizer_web_rubberband_live128.cpp
    rubberband_live128
  )
  add_harmonizer_backend(
    harmonizer_web_signalsmith
    backends/harmonizer_web_signalsmith.cpp
    signalsmith-stretch
  )

  install(TARGETS
    harmonizer_web_parallel
    harmonizer_web_rubberband_live128
    harmonizer_web_rubberband_r2
    harmonizer_web_signalsmith
    RUNTIME DESTINATION .
  )

  add_executable(harmonizer_pitch_analyzer pitch_analyzer.cpp)
  target_link_libraries(harmonizer_pitch_analyzer PRIVATE PkgConfig::AUBIO)
  set_target_properties(harmonizer_pitch_analyzer PROPERTIES OUTPUT_NAME pitch_analyzer)

  add_executable(harmonizer_spectrum_analyzer spectrum_analyzer.cpp)
  target_compile_options(harmonizer_spectrum_analyzer PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra>
  )
  set_target_properties(harmonizer_spectrum_analyzer PROPERTIES OUTPUT_NAME spectrum_analyzer)

  add_executable(harmonizer_polyphonic_analyzer
    polyphonic_analyzer.cpp
    analysis_audio.hpp
  )
  target_compile_options(harmonizer_polyphonic_analyzer PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra>
  )
  set_target_properties(harmonizer_polyphonic_analyzer PROPERTIES
    OUTPUT_NAME polyphonic_analyzer
  )

  add_executable(harmonizer_formant_analyzer
    formant_analyzer.cpp
    analysis_audio.hpp
  )
  target_compile_options(harmonizer_formant_analyzer PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra>
  )
  set_target_properties(harmonizer_formant_analyzer PROPERTIES
    OUTPUT_NAME formant_analyzer
  )

  add_executable(harmonizer_latency_probe
    latency_probe.cpp
    analysis_audio.hpp
  )
  target_compile_options(harmonizer_latency_probe PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra>
  )
  target_link_libraries(harmonizer_latency_probe PRIVATE PkgConfig::PORTAUDIO)
  set_target_properties(harmonizer_latency_probe PROPERTIES
    OUTPUT_NAME latency_probe
  )
  add_test(
    NAME latency_probe_waveform_self_test
    COMMAND harmonizer_latency_probe --self-test --mode waveform
  )
  add_test(
    NAME latency_probe_envelope_self_test
    COMMAND harmonizer_latency_probe --self-test --mode envelope
  )
endif()

install(TARGETS harmonizer_web RUNTIME DESTINATION .)
install(FILES web/index.html DESTINATION web)
install(FILES
  README.md
  QUICKSTART.md
  LICENSE
  THIRD_PARTY_NOTICES.md
  VERSION.txt
  DESTINATION .
)
