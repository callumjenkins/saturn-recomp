# The saturn-recomp runtime, included by the generated CMakeLists.txt.
#   saturn_core  the work RAMs, the address map outside them, dispatch over the modules
#   saturn_stub  no hardware: tests that only call the game's own code
#   saturn_hw    the Saturn: machine, BIOS, SCU, SMPC, CD block, on-chip, VDP1, VDP2, the
#                   SCSP and its 68000, and the host window (SDL3, OpenGL 4.5, audio)
#   saturn_m68k  Musashi, the 68000 (third_party/musashi, C)
# A target links saturn_core, one of saturn_stub / saturn_hw, then recomp.
set(_rt ${CMAKE_CURRENT_LIST_DIR})
add_library(saturn_core OBJECT ${_rt}/core.cpp)
add_library(saturn_stub OBJECT ${_rt}/services_stub.cpp)
add_library(saturn_hw OBJECT
    ${_rt}/machine.cpp ${_rt}/bios.cpp ${_rt}/mmio.cpp ${_rt}/scu.cpp ${_rt}/scudsp.cpp ${_rt}/smpc.cpp
    ${_rt}/cdrom.cpp ${_rt}/cdblock.cpp ${_rt}/onchip.cpp ${_rt}/video.cpp ${_rt}/vdp1.cpp ${_rt}/vdp2.cpp
    ${_rt}/host.cpp ${_rt}/sound.cpp ${_rt}/scsp.cpp ${_rt}/tasks.cpp)
enable_language(C)
set(_m68k ${_rt}/third_party/musashi)
add_library(saturn_m68k STATIC
    ${_m68k}/m68kcpu.c ${_m68k}/m68kops.c ${_m68k}/m68kdasm.c ${_m68k}/softfloat/softfloat.c)
target_include_directories(saturn_m68k PUBLIC ${_m68k})
target_compile_options(saturn_m68k PRIVATE -w)
find_package(SDL3 REQUIRED CONFIG)
target_link_libraries(saturn_hw PUBLIC SDL3::SDL3 saturn_m68k)
foreach(t saturn_core saturn_stub saturn_hw)
  target_include_directories(${t} PUBLIC ${_rt})
endforeach()
find_package(Threads REQUIRED)
