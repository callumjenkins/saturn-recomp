# The runtime's unit tests, built on their own: no recompiled game, no SDL needed.
#   cmake -S runtime/tests -B build/runtime-tests -G Ninja -DCMAKE_CXX_COMPILER=clang++
#   ninja -C build/runtime-tests && build/runtime-tests/runtime_tests
cmake_minimum_required(VERSION 3.20)
project(saturn_runtime_tests CXX)
set(CMAKE_CXX_STANDARD 20)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../runtime.cmake)
add_executable(runtime_tests main.cpp no_modules.cpp vdp2_test.cpp bup_test.cpp pads_test.cpp settings_test.cpp touch_test.cpp)
target_link_libraries(runtime_tests saturn_core saturn_machine saturn_host Threads::Threads)
