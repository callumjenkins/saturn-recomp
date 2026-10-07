# The saturn-recomp runtime, included by the generated CMakeLists.txt.
#   saturn_core     the work RAMs, the address map outside them, dispatch over the modules
#   saturn_stub     no hardware: tests that only call the game's own code
#   saturn_machine  the Saturn: machine, BIOS, SCU, SMPC, CD block, on-chip, VDP1, VDP2, the
#                   SCSP and its 68000; no SDL
#   saturn_host     the window, the pad, the pace and the audio stream (SDL3, OpenGL 4.5); without
#                   SDL3, or with -DSATURN_NO_SDL=ON, a host for headless runs only
#   saturn_m68k     Musashi, the 68000 (third_party/musashi, C)
# A target links saturn_core, then saturn_stub or saturn_machine with saturn_host, then recomp.
set(_rt ${CMAKE_CURRENT_LIST_DIR})
set(_src ${_rt}/src)
set(_internal ${_src}/machine ${_src}/video ${_src}/sound ${_src}/host)
add_library(saturn_core OBJECT ${_src}/core/core.cpp)
add_library(saturn_stub OBJECT ${_src}/stub/services_stub.cpp)
add_library(saturn_machine OBJECT
    ${_src}/machine/machine.cpp ${_src}/machine/bios.cpp ${_src}/machine/mmio.cpp ${_src}/machine/tasks.cpp
    ${_src}/machine/onchip.cpp ${_src}/machine/scu.cpp ${_src}/machine/scudsp.cpp ${_src}/machine/smpc.cpp
    ${_src}/machine/cdrom.cpp ${_src}/machine/cdblock.cpp ${_src}/machine/agent.cpp
    ${_src}/video/video.cpp ${_src}/video/vdp1.cpp ${_src}/video/vdp2.cpp ${_src}/video/png.cpp ${_src}/video/movie.cpp
    ${_src}/sound/sound.cpp ${_src}/sound/scsp.cpp)
option(SATURN_NO_SDL "build the headless host even if SDL3 is installed" OFF)
# VDP1's shapes drawn by Mednafen's rules (vdp1_raster_gpl.inc): the build is then under the GPL
option(SATURN_VDP1_GPL "draw VDP1's shapes by Mednafen's rules, making the build a GPL work" OFF)
if(SATURN_VDP1_GPL)
  target_compile_definitions(saturn_machine PRIVATE SATURN_VDP1_GPL=1)
endif()
if(NOT SATURN_NO_SDL)
  find_package(SDL3 CONFIG)
endif()
if(SDL3_FOUND)
  add_library(saturn_host OBJECT ${_src}/host/host.cpp ${_src}/host/settings.cpp)
  target_link_libraries(saturn_host PUBLIC SDL3::SDL3)
else()
  add_library(saturn_host OBJECT ${_src}/host/host_null.cpp ${_src}/host/settings.cpp)
endif()
enable_language(C)
set(_m68k ${_rt}/third_party/musashi)
add_library(saturn_m68k STATIC
    ${_m68k}/m68kcpu.c ${_m68k}/m68kops.c ${_m68k}/m68kdasm.c ${_m68k}/softfloat/softfloat.c)
target_include_directories(saturn_m68k PUBLIC ${_m68k})
target_compile_options(saturn_m68k PRIVATE -w)
target_link_libraries(saturn_machine PUBLIC saturn_m68k)
foreach(t saturn_core saturn_stub saturn_machine saturn_host)
  target_include_directories(${t} PUBLIC ${_rt}/include)
endforeach()
foreach(t saturn_machine saturn_host)
  target_include_directories(${t} PUBLIC ${_internal})
endforeach()
find_package(Threads REQUIRED)
