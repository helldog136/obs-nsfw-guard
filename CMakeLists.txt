cmake_minimum_required(VERSION 3.28)

project(obs-nsfw-guard VERSION 0.1.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

set(DEPS ${CMAKE_SOURCE_DIR}/deps)

add_library(obs-nsfw-guard MODULE
  src/plugin-main.cpp
  src/nsfw-filter.cpp
  src/classifier.cpp
)

target_include_directories(obs-nsfw-guard PRIVATE
  ${DEPS}/obs-src/libobs
  ${DEPS}/ort/build/native/include
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/compat
)

# obs.lib est générée depuis l'obs.dll installé (voir deps/obs.def).
target_link_libraries(obs-nsfw-guard PRIVATE ${DEPS}/obs.lib)

target_compile_definitions(obs-nsfw-guard PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(obs-nsfw-guard PRIVATE /utf-8 /W3 /Zc:__cplusplus /Zc:preprocessor)

# Empaquetage : stage/<layout d'un plugin OBS utilisateur>
set(STAGE ${CMAKE_SOURCE_DIR}/stage/obs-nsfw-guard)
add_custom_command(TARGET obs-nsfw-guard POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/bin/64bit ${STAGE}/data/locale
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:obs-nsfw-guard> ${STAGE}/bin/64bit/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${DEPS}/ort/runtimes/win-x64/native/onnxruntime.dll
          ${DEPS}/ort/runtimes/win-x64/native/onnxruntime_providers_shared.dll
          ${STAGE}/bin/64bit/
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/models/mobilenetv4_conv_small.e2400_r224_in1k_nsfw_classifier.onnx
          ${STAGE}/data/nsfw_model.onnx
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data/locale ${STAGE}/data/locale
)
