# The saturn-recomp runtime, included by the generated CMakeLists.txt.
#   saturn_core     the work RAMs, the address map outside them, dispatch over the modules
#   saturn_stub     no hardware: tests that only call the game's own code
#   saturn_machine  the Saturn: machine, BIOS, SCU, SMPC, CD block, on-chip, VDP1, VDP2, the
#                   SCSP and its 68000; no SDL
#   saturn_host     the launcher, the window, the pad, the pace and the audio stream (SDL3); without
#                   SDL3, or with -DSATURN_NO_SDL=ON, a host for headless runs only
#   saturn_m68k     Musashi, the 68000 (third_party/musashi, C)
#   saturn_imgui    Dear ImGui, the launcher's menu (third_party/imgui), with SDL3 only
#   saturn_net      sessions, the peers in a playtest room (net.cmake); with -DSATURN_NO_NET=ON, a session that
#                   never opens
# The in-game menu and the touch pad draw text in Inter (third_party/inter), with stb_truetype (third_party/stb).
# A target links saturn_core, then saturn_stub or saturn_machine with saturn_host, then recomp.
set(_rt ${CMAKE_CURRENT_LIST_DIR})
set(_src ${_rt}/src)
set(_internal ${_src}/machine ${_src}/video ${_src}/sound ${_src}/host)
add_library(saturn_core OBJECT ${_src}/core/core.cpp)
add_library(saturn_stub OBJECT ${_src}/stub/services_stub.cpp)
add_library(saturn_machine OBJECT
    ${_src}/machine/machine.cpp ${_src}/machine/bios.cpp ${_src}/machine/mmio.cpp ${_src}/machine/tasks.cpp
    ${_src}/machine/onchip.cpp ${_src}/machine/scu.cpp ${_src}/machine/scudsp.cpp ${_src}/machine/smpc.cpp
    ${_src}/machine/cdrom.cpp ${_src}/machine/cdblock.cpp ${_src}/machine/agent.cpp ${_src}/machine/state.cpp
    ${_src}/machine/modem.cpp ${_src}/machine/modem_tcp.cpp ${_src}/machine/line_log.cpp ${_src}/machine/modem_session.cpp ${_src}/machine/online.cpp ${_src}/machine/online_room.cpp
    ${_src}/video/video.cpp ${_src}/video/vdp1.cpp ${_src}/video/vdp2.cpp ${_src}/video/png.cpp ${_src}/video/movie.cpp
    ${_src}/sound/sound.cpp ${_src}/sound/scsp.cpp)
option(SATURN_NO_SDL "build the headless host even if SDL3 is installed" OFF)
# VDP1's shapes drawn by Mednafen's rules (vdp1_raster_gpl.inc): the build is then under the GPL
option(SATURN_VDP1_GPL "draw VDP1's shapes by Mednafen's rules, making the build a GPL work" OFF)
if(SATURN_VDP1_GPL)
  target_compile_definitions(saturn_machine PRIVATE SATURN_VDP1_GPL=1)
endif()
option(SATURN_REQUIRE_SDL "stop if SDL3 is not found, rather than building the headless host" OFF)
if(NOT SATURN_NO_SDL)
  find_package(SDL3 CONFIG)
endif()
if(SATURN_REQUIRE_SDL AND NOT SDL3_FOUND)
  message(FATAL_ERROR "SATURN_REQUIRE_SDL is on but SDL3 was not found (set CMAKE_PREFIX_PATH to its install)")
endif()
if(SDL3_FOUND)
  set(_imgui ${_rt}/third_party/imgui)
  add_library(saturn_imgui STATIC
      ${_imgui}/imgui.cpp ${_imgui}/imgui_draw.cpp ${_imgui}/imgui_tables.cpp ${_imgui}/imgui_widgets.cpp
      ${_imgui}/backends/imgui_impl_sdl3.cpp ${_imgui}/backends/imgui_impl_sdlrenderer3.cpp)
  target_include_directories(saturn_imgui PUBLIC ${_imgui} ${_imgui}/backends)
  target_link_libraries(saturn_imgui PUBLIC SDL3::SDL3)
  target_compile_options(saturn_imgui PRIVATE -w)
  # the menu's font, compiled in
  set(_fonts ${CMAKE_CURRENT_BINARY_DIR}/saturn-fonts)
  foreach(weight Regular SemiBold)
    string(TOLOWER ${weight} f)
    set(_ttf ${_rt}/third_party/inter/Inter-${weight}.ttf)
    add_custom_command(OUTPUT ${_fonts}/inter_${f}.h
        COMMAND ${CMAKE_COMMAND} -DINPUT=${_ttf} -DOUTPUT=${_fonts}/inter_${f}.h -DNAME=inter_${f} -P ${_rt}/embed.cmake
        DEPENDS ${_ttf} ${_rt}/embed.cmake VERBATIM)
    list(APPEND _font_headers ${_fonts}/inter_${f}.h)
  endforeach()
  add_library(saturn_host OBJECT ${_src}/host/host.cpp ${_src}/host/launcher.cpp ${_src}/host/online_card.cpp ${_src}/host/settings.cpp ${_src}/host/ui.cpp
      ${_src}/host/uinput_pad.cpp ${_font_headers})
  target_include_directories(saturn_host PRIVATE ${_fonts} ${_rt}/third_party/stb)
  target_link_libraries(saturn_host PUBLIC SDL3::SDL3 saturn_imgui)
else()
  add_library(saturn_host OBJECT ${_src}/host/host_null.cpp ${_src}/host/settings.cpp)
endif()
if(WIN32)
  target_sources(saturn_host INTERFACE ${_src}/host/windows/saturn.manifest)   # merged into each executable's
endif()
enable_language(C)
set(_m68k ${_rt}/third_party/musashi)
add_library(saturn_m68k STATIC
    ${_m68k}/m68kcpu.c ${_m68k}/m68kops.c ${_m68k}/m68kdasm.c ${_m68k}/softfloat/softfloat.c
    ${_src}/sound/m68k_state.c)
target_include_directories(saturn_m68k PUBLIC ${_m68k})
target_include_directories(saturn_machine PRIVATE ${_rt}/third_party/minicoro)
target_compile_options(saturn_m68k PRIVATE -w)
target_link_libraries(saturn_machine PUBLIC saturn_m68k)
if(WIN32)
  target_link_libraries(saturn_machine PUBLIC ws2_32)   # the modem's TCP line
endif()
option(SATURN_NO_NET "build without network play, and without fetching libdatachannel and Mbed TLS" OFF)
if(SATURN_NO_NET)
  add_library(saturn_net STATIC ${_src}/net/session_none.cpp)
  target_include_directories(saturn_net PUBLIC ${_src}/net)
else()
  include(${_rt}/net.cmake)
endif()
target_link_libraries(saturn_machine PUBLIC saturn_net)
# The builds a room compares (net/session.h's terms), looked up on every build (build_id.cmake)
set(SATURN_GAME_DIR "" CACHE PATH "the game's checkout, whose commit names the game's build")
set(SATURN_GAME_BUILD "" CACHE STRING "the game's build, in place of its checkout's commit")
set(_build_id ${CMAKE_CURRENT_BINARY_DIR}/saturn-build-id/saturn_build_id.h)
add_custom_target(saturn_build_id BYPRODUCTS ${_build_id}
    COMMAND ${CMAKE_COMMAND} -DRUNTIME_DIR=${_rt} -DGAME_DIR=${SATURN_GAME_DIR} -DGAME_BUILD=${SATURN_GAME_BUILD}
            -DOUTPUT=${_build_id} -P ${_rt}/build_id.cmake VERBATIM)
add_dependencies(saturn_machine saturn_build_id)
target_include_directories(saturn_machine PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/saturn-build-id)
foreach(t saturn_core saturn_stub saturn_machine saturn_host)
  target_include_directories(${t} PUBLIC ${_rt}/include)
endforeach()
foreach(t saturn_machine saturn_host)
  target_include_directories(${t} PUBLIC ${_internal})
endforeach()
find_package(Threads REQUIRED)
