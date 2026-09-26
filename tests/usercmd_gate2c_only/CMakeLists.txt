# Standalone build for usercmd_gate2c_only_test.cpp -- deliberately NOT
# referenced by the top-level CMakeLists.txt (add_subdirectory is never
# called on this directory from there), so it has no effect whatsoever
# on the main BotController plugin build or on .github/workflows/build.yml.
#
# This exercises a hand-verified mirror of InputInjector.cpp's Gate2C-only
# movement-sink suppression logic with zero CS2/HL2SDK/KHook/protobuf
# dependency -- see usercmd_gate2c_only_test.cpp's own top comment for
# exactly what is mirrored and why InputInjector.cpp cannot be compiled
# standalone the way tests/gate2c compiles Gate2CDiagnostics.cpp directly.
#
# To build and run it standalone (once authorized):
#   cmake -S tests/usercmd_gate2c_only -B build/usercmd-gate2c-only-tests
#   cmake --build build/usercmd-gate2c-only-tests
#   ctest --test-dir build/usercmd-gate2c-only-tests --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(UsercmdGate2COnlyTest CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(usercmd_gate2c_only_test
    usercmd_gate2c_only_test.cpp
)

find_package(Threads REQUIRED)
target_link_libraries(usercmd_gate2c_only_test PRIVATE Threads::Threads)

enable_testing()
add_test(NAME usercmd_gate2c_only_test COMMAND usercmd_gate2c_only_test)
