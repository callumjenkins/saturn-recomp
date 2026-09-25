# The saturnkit runtime, included by the generated CMakeLists.txt.
#   saturnkit_core  the work RAMs, the address map outside them, dispatch over the modules
#   saturnkit_stub  no hardware: tests that only call the game's own code
#   saturnkit_hw    the Saturn: machine, BIOS, SCU, SMPC, CD block, on-chip, video (as memory)
# A target links saturnkit_core, one of saturnkit_stub / saturnkit_hw, then recomp.
set(_rt ${CMAKE_CURRENT_LIST_DIR})
add_library(saturnkit_core OBJECT ${_rt}/core.cpp)
add_library(saturnkit_stub OBJECT ${_rt}/services_stub.cpp)
add_library(saturnkit_hw OBJECT
    ${_rt}/machine.cpp ${_rt}/bios.cpp ${_rt}/mmio.cpp ${_rt}/scu.cpp ${_rt}/smpc.cpp
    ${_rt}/cdrom.cpp ${_rt}/cdblock.cpp ${_rt}/onchip.cpp ${_rt}/video.cpp)
foreach(t saturnkit_core saturnkit_stub saturnkit_hw)
  target_include_directories(${t} PUBLIC ${_rt})
endforeach()
find_package(Threads REQUIRED)
