# Standalone build for gate2c_lifecycle_test.cpp -- deliberately NOT
# referenced by the top-level CMakeLists.txt (add_subdirectory is never
# called on this directory from there), so it has no effect whatsoever
# on the main BotController plugin build or on .github/workflows/build.yml.
#
# This exists so Gate2CDiagnostics.cpp's positional PRE/POST pairing
# state machine can be exercised with zero CS2/HL2SDK/KHook dependency
# (see gate2c_lifecycle_test.cpp's own top comment). It is not built or
# run as part of this change -- per the review that authorized this
# design, running it requires separate authorization.
#
# To build and run it standalone (once authorized):
#   cmake -S tests/gate2c -B build/gate2c-tests
#   cmake --build build/gate2c-tests
#   ctest --test-dir build/gate2c-tests --output-on-failure
cmake_minimum_required(VERSION 3.20)
project(Gate2CLifecycleTest CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")

add_executable(gate2c_lifecycle_test
    gate2c_lifecycle_test.cpp
    "${BC_ROOT}/src/features/recorder/Gate2CDiagnostics.cpp"
)

target_include_directories(gate2c_lifecycle_test PRIVATE
    "${BC_ROOT}/src/features/recorder"
    "${BC_ROOT}/src/core"
)

find_package(Threads REQUIRED)
target_link_libraries(gate2c_lifecycle_test PRIVATE Threads::Threads)

enable_testing()
add_test(NAME gate2c_lifecycle_test COMMAND gate2c_lifecycle_test)
