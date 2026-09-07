# SuprNAM — the one Supr pedal that needs a real build system.
#
# The other sixteen plugins are dependency-free header DSP and stay in the
# Makefile. This one links NeuralAudio (and through it NAM Core, RTNeural,
# Eigen and math_approx), all of which are CMake projects, so it gets its own.
#
#   git submodule update --init --recursive
#   cmake -B build/nam -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/nam -j4
#   sudo cmake --install build/nam
#
# On a Pi 4, add -DSUPR_CPU=cortex-a72 (or cortex-a76 on a Pi 5). It is worth
# doing: the inner loops are all NEON and the difference is not small.

cmake_minimum_required(VERSION 3.16)
project(SuprNam VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Absolute override for non-standard layouts. Left empty by default so the
# install destination stays *relative*, which is what makes both
# `cmake --install --prefix ...` and DESTDIR work — baking
# ${CMAKE_INSTALL_PREFIX} into a cache variable here would freeze the path at
# configure time and silently ignore either of them.
set(LV2_DIR "" CACHE PATH "Absolute LV2 bundle directory (default: <prefix>/lib/lv2)")
set(SUPR_CPU "" CACHE STRING "Target CPU for -mcpu (e.g. cortex-a72 on a Pi 4)")

# --- LV2 headers -------------------------------------------------------------
# Unlike the Makefile plugins, SuprNAM needs the atom, patch, worker, state and
# buf-size extension headers, which the vendored single-header copy does not
# carry. Requires lv2-dev (apt) or lv2 (brew).
find_package(PkgConfig REQUIRED)
pkg_check_modules(LV2 REQUIRED lv2)

# --- NeuralAudio -------------------------------------------------------------
# Pull in the library target only, not the C API or the utils.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/modules/NeuralAudio/NeuralAudio/CMakeLists.txt")
    message(FATAL_ERROR
        "modules/NeuralAudio is empty. Run: git submodule update --init --recursive")
endif()
add_subdirectory(modules/NeuralAudio/NeuralAudio NeuralAudio)

# --- SuprNAM -----------------------------------------------------------------
add_library(suprnam MODULE
    src/nam/SuprNam.cpp
    src/nam/NamModel.cpp
    src/nam/NamEngine.cpp
    src/vendor/lv2_plugin/Lv2Plugin.cpp
)

set_target_properties(suprnam PROPERTIES PREFIX "" OUTPUT_NAME "suprnam")

target_include_directories(suprnam PRIVATE src)
# SYSTEM so upstream's warnings stay out of ours. The vendored framework
# includes itself as lv2_plugin/..., so src/vendor is the root, not src/vendor/lv2_plugin.
target_include_directories(suprnam SYSTEM PRIVATE
    src/vendor
    ${LV2_INCLUDE_DIRS}
)

target_compile_options(suprnam PRIVATE
    -Wall -Wextra
    $<$<CONFIG:Release>:-O3>
)

# -ffast-math is deliberately absent. The rest of the library uses it happily,
# but here it would let the compiler reassociate the arithmetic inside the
# parallel paths, and two paths that are meant to cancel exactly are worth more
# than the couple of percent it would buy.
if(SUPR_CPU)
    target_compile_options(suprnam PRIVATE -mcpu=${SUPR_CPU})
endif()

target_link_libraries(suprnam PRIVATE NeuralAudio ${LV2_LIBRARIES} pthread)

# Export only lv2_descriptor. See suprnam.version — without this the statically
# linked NeuralAudio and NAM Core symbols go global and interpose on any other
# plugin carrying its own build of them, which broke TooB NAM whenever SuprNAM
# happened to load first. GNU ld syntax, so Linux only; macOS builds are for
# the offline tests and never share a process with another NAM plugin.
if(NOT APPLE)
    target_link_options(suprnam PRIVATE
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/suprnam.version")
endif()

# --- Install -----------------------------------------------------------------
if(LV2_DIR)
    set(SUPRNAM_BUNDLE "${LV2_DIR}/suprnam.lv2")
else()
    set(SUPRNAM_BUNDLE "lib/lv2/suprnam.lv2")
endif()

install(TARGETS suprnam LIBRARY DESTINATION "${SUPRNAM_BUNDLE}")
install(FILES ttl/suprnam.ttl DESTINATION "${SUPRNAM_BUNDLE}")
install(FILES ttl/nam-manifest.ttl DESTINATION "${SUPRNAM_BUNDLE}" RENAME manifest.ttl)

# --- Measurement rig ---------------------------------------------------------
# Not part of the plugin, and not a test: this is the tool that takes the
# measurements an analytic pedal gets fitted to. SuprSans was built this way
# against SansAmp captures; SuprFuzz is being built the same way against Big
# Muff captures, so the rig lives in the repo rather than being rewritten each
# time. Needs NeuralAudio, hence its home here rather than in the Makefile.
option(SUPR_BUILD_TOOLS "Build the offline measurement tools" ON)
if(SUPR_BUILD_TOOLS)
    add_executable(nam_probe tools/nam_probe.cpp src/nam/NamModel.cpp)
    target_include_directories(nam_probe PRIVATE src)
    target_link_libraries(nam_probe PRIVATE NeuralAudio pthread)
endif()

# --- Offline graph tests -----------------------------------------------------
# No LV2 and no NeuralAudio: this is the pure routing/mixing/threading graph,
# so it builds and runs on a development machine.
option(SUPR_BUILD_TESTS "Build the SuprNAM offline tests" ON)
if(SUPR_BUILD_TESTS)
    add_executable(test_nam test/test_nam.cpp src/nam/NamEngine.cpp)
    target_include_directories(test_nam PRIVATE src)
    target_link_libraries(test_nam PRIVATE pthread)

    # Loads the built bundle through the real LV2 entry points. This is the
    # only test that can catch ttl/suprnam.ttl and the PortIndex enum drifting
    # apart, which produces no error at all — just a Blend knob that turns out
    # to be Output Gain.
    add_executable(test_nam_host test/test_nam_host.cpp)
    target_include_directories(test_nam_host SYSTEM PRIVATE ${LV2_INCLUDE_DIRS})
    target_link_libraries(test_nam_host PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(test_nam_host suprnam)

    # Real inference against real .nam files. Skips itself when models/ has
    # fewer than two files in it.
    add_executable(test_nam_models
        test/test_nam_models.cpp src/nam/NamEngine.cpp src/nam/NamModel.cpp)
    target_include_directories(test_nam_models PRIVATE src)
    target_link_libraries(test_nam_models PRIVATE NeuralAudio pthread)

    enable_testing()
    add_test(NAME nam_graph COMMAND test_nam)
    add_test(NAME nam_models COMMAND test_nam_models "${CMAKE_CURRENT_SOURCE_DIR}/models")
    add_test(NAME nam_host COMMAND test_nam_host
             $<TARGET_FILE:suprnam> "${CMAKE_CURRENT_SOURCE_DIR}/ttl/suprnam.ttl")
endif()
