# Writes OUTPUT, saturn_build_id.h, with the builds a session's terms name: the runtime's and the game's, each as
# RUNTIME_BUILD or GAME_BUILD gives it or else its checkout's commit, "dev" when neither is known. A copy built from
# files without git, as on another machine, takes both from the checkout it was copied from.
# Rewritten only when they change.
#   cmake -DRUNTIME_DIR=... -DRUNTIME_BUILD=... -DGAME_DIR=... -DGAME_BUILD=... -DOUTPUT=... -P build_id.cmake
function(commit_of dir out)
  set(${out} "dev" PARENT_SCOPE)
  if(dir)
    execute_process(COMMAND git -C ${dir} rev-parse --short=7 HEAD OUTPUT_VARIABLE c
        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE failed)
    if(NOT failed AND c)
      set(${out} ${c} PARENT_SCOPE)
    endif()
  endif()
endfunction()
if(RUNTIME_BUILD)
  set(runtime ${RUNTIME_BUILD})
else()
  commit_of("${RUNTIME_DIR}" runtime)
endif()
if(GAME_BUILD)
  set(game ${GAME_BUILD})
else()
  commit_of("${GAME_DIR}" game)
endif()
set(text "#pragma once\n#define SATURN_RUNTIME_BUILD \"${runtime}\"\n#define SATURN_GAME_BUILD \"${game}\"\n")
if(EXISTS ${OUTPUT})
  file(READ ${OUTPUT} old)
endif()
if(NOT old STREQUAL text)
  file(WRITE ${OUTPUT} "${text}")
endif()
