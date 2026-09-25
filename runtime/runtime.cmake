# The saturnkit runtime, included by the generated CMakeLists.txt.
#   saturnkit_core  the work RAMs, the address map outside them, dispatch over the modules
#   saturnkit_stub  no hardware: tests that only call the game's own code
# A target links saturnkit_core, saturnkit_stub (or, later, the hardware), then recomp.
set(_rt ${CMAKE_CURRENT_LIST_DIR})
add_library(saturnkit_core OBJECT ${_rt}/core.cpp)
add_library(saturnkit_stub OBJECT ${_rt}/services_stub.cpp)
foreach(t saturnkit_core saturnkit_stub)
  target_include_directories(${t} PUBLIC ${_rt})
endforeach()
