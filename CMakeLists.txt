# OneGrade — cross-platform OpenFX plugin build (macOS / Windows / Linux).
# SPDX-License-Identifier: BSD-3-Clause
#
#   cmake -S . -B build-cmake [-DBUILD_CUDA=ON]
#   cmake --build build-cmake --config Release
#   ctest --test-dir build-cmake
#
# Output: build-cmake/OneGrade.ofx.bundle
cmake_minimum_required(VERSION 3.20)
project(OneGrade LANGUAGES CXX)

option(BUILD_CUDA "Compile the CUDA kernel and advertise CUDA render" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(APPLE)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "" FORCE)
endif()

set(SDK "${CMAKE_SOURCE_DIR}/third_party/openfx")

set(SUPPORT_SRCS
  ${SDK}/Support/Library/ofxsCore.cpp
  ${SDK}/Support/Library/ofxsImageEffect.cpp
  ${SDK}/Support/Library/ofxsInteract.cpp
  ${SDK}/Support/Library/ofxsLog.cpp
  ${SDK}/Support/Library/ofxsMultiThread.cpp
  ${SDK}/Support/Library/ofxsParams.cpp
  ${SDK}/Support/Library/ofxsProperty.cpp
  ${SDK}/Support/Library/ofxsPropertyValidation.cpp)

set(PLUGIN_SRCS src/OneGrade.cpp src/OpenCLKernel.cpp)

find_package(OpenCL REQUIRED)

if(APPLE)
  list(APPEND PLUGIN_SRCS src/MetalKernel.mm)
  set(ARCHDIR "MacOS")
elseif(WIN32)
  set(ARCHDIR "Win64")
else()
  set(ARCHDIR "Linux-x86-64")
endif()

if(BUILD_CUDA)
  # Forwarded to nvcc as -arch=all-major: SASS for every major arch the *toolkit*
  # supports, plus PTX for the newest so a future GPU JITs rather than leaving
  # Resolve to silently render this node on the CPU. Do not expand this to an
  # explicit list from CMAKE_CUDA_ARCHITECTURES_ALL_MAJOR — that variable is
  # baked into CMake, so it lags the toolkit (CMake 3.31 still lists the
  # compute_50 that CUDA 13 removed, and stops at 90). Must be set before
  # enable_language(CUDA), which initialises the target property.
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES all-major)
  endif()
  enable_language(CUDA)
  list(APPEND PLUGIN_SRCS src/CudaKernel.cu)
endif()

# ncnn: the inference runtime for Magic Grade's region masks. Vendored and built from source
# (third_party/ncnn/VENDORED.md) so CI needs no package manager, no vcpkg entry and no network
# step beyond the checkout itself.
#
# Vulkan and OpenMP are both OFF on purpose. Vulkan because inference here is a button press on
# the CPU rather than part of a render, and OpenMP because it would put a libomp dependency
# inside a plugin bundle that has to load on machines we do not control.
set(NCNN_VULKAN         OFF CACHE BOOL "" FORCE)
set(NCNN_OPENMP         OFF CACHE BOOL "" FORCE)
set(NCNN_SHARED_LIB     OFF CACHE BOOL "" FORCE)
set(NCNN_SIMPLEOCV      OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TOOLS    OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(NCNN_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/ncnn EXCLUDE_FROM_ALL)

add_library(OneGrade MODULE ${PLUGIN_SRCS} ${SUPPORT_SRCS})
target_include_directories(OneGrade PRIVATE
  ${SDK}/include ${SDK}/Support/include ${SDK}/Support/Plugins/include src)
# OPENCL, not just OPENGL: the two are unrelated flags and the near-identical
# names hid a real bug. OFX_SUPPORTS_OPENCLRENDER guards the body of
# processImagesOpenCL(), so leaving it undefined compiles the OpenCL render to
# an empty function while the plugin still advertises OpenCL support.
# OpenCLKernel.cpp and OpenCL::OpenCL are always built, so this is unconditional.
target_compile_definitions(OneGrade PRIVATE OFX_SUPPORTS_OPENGLRENDER OFX_SUPPORTS_OPENCLRENDER)
target_link_libraries(OneGrade PRIVATE OpenCL::OpenCL ncnn)

set_target_properties(OneGrade PROPERTIES PREFIX "" SUFFIX ".ofx" OUTPUT_NAME "OneGrade")

if(APPLE)
  target_link_libraries(OneGrade PRIVATE "-framework Metal" "-framework AppKit")
endif()
if(BUILD_CUDA)
  target_compile_definitions(OneGrade PRIVATE OFX_SUPPORTS_CUDARENDER)
  # Whole-program (not CUDA_SEPARABLE_COMPILATION): CudaKernel.cu is one TU with
  # no cross-TU device calls, and the -rdc=true device-link that separable
  # compilation forces would strip the fatbinary's PTX, costing forward compat.
endif()

# ---- assemble the .ofx.bundle next to the built module ----
set(BUNDLE "${CMAKE_BINARY_DIR}/OneGrade.ofx.bundle")
add_custom_command(TARGET OneGrade POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE}/Contents/${ARCHDIR}"
  COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:OneGrade>" "${BUNDLE}/Contents/${ARCHDIR}/OneGrade.ofx"
  COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_SOURCE_DIR}/src/Info.plist" "${BUNDLE}/Contents/Info.plist"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${BUNDLE}/Contents/Resources/LUTs"
  COMMAND ${CMAKE_COMMAND} -E copy
          "${CMAKE_SOURCE_DIR}/luts/OneGrade Desert Day.cube"
          "${CMAKE_SOURCE_DIR}/luts/OneGrade Cinematic Landscape.cube"
          "${CMAKE_SOURCE_DIR}/luts/OneGrade Golden Hour.cube"
          "${CMAKE_SOURCE_DIR}/luts/OneGrade Teal Orange.cube"
          "${CMAKE_SOURCE_DIR}/luts/OneGrade Silver Bleach.cube"
          "${CMAKE_SOURCE_DIR}/luts/OneGrade Midnight Blue.cube"
          "${BUNDLE}/Contents/Resources/LUTs/"
  COMMENT "Assembling OneGrade.ofx.bundle")

# ---- CPU unit tests ----
enable_testing()
add_executable(pipeline_test test/pipeline_test.cpp)
add_test(NAME pipeline COMMAND pipeline_test)
