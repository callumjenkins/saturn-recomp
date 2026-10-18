#!/bin/bash
# Every check that needs no game data: the Python tests, the recompiler's instruction self-test and
# the runtime's unit tests, all built without SDL. CI runs this; so can you, before a commit.
# With SATURN_SDL set to an SDL3 install prefix, it also builds the self-test's program on the SDL host.
set -euo pipefail
cd "$(dirname "$0")/.."
B=build/check
CMAKE=(-G Ninja -DCMAKE_CXX_COMPILER="${CXX:-clang++}" -DSATURN_NO_SDL=ON)
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) WINDOWS=1 ;; *) WINDOWS= ;; esac
# Microsoft's C runtime linked in, since a fresh Windows has no vcruntime140.dll or msvcp140.dll
CRT=(-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded)
CMAKE+=("${CRT[@]}")
mkdir -p $B

# A step's output only when it fails.
quiet() { "$@" > $B/step.log 2>&1 || { cat $B/step.log; return 1; }; }

echo "== Python tests"
uv run pytest -q

echo "== instruction self-test"
quiet uv run python -m saturnrecomp.recomp --out $B/optest --optest
quiet cmake -S $B/optest -B $B/optest-build "${CMAKE[@]}"
quiet ninja -C $B/optest-build selftest saturn
$B/optest-build/selftest $B/optest/selftest/optest.txt

echo "== runtime unit tests"
quiet cmake -S runtime/tests -B $B/runtime-tests "${CMAKE[@]}"
quiet ninja -C $B/runtime-tests
$B/runtime-tests/runtime_tests

echo "== runtime unit tests, VDP1 by Mednafen's rules (SATURN_VDP1_GPL)"
quiet cmake -S runtime/tests -B $B/runtime-tests-gpl "${CMAKE[@]}" -DSATURN_VDP1_GPL=ON
quiet ninja -C $B/runtime-tests-gpl
$B/runtime-tests-gpl/runtime_tests

if [ -n "${SATURN_SDL:-}" ]; then
    echo "== the SDL host"
    quiet cmake -S $B/optest -B $B/optest-sdl -G Ninja -DCMAKE_CXX_COMPILER="${CXX:-clang++}" -DSATURN_REQUIRE_SDL=ON \
        -DCMAKE_PREFIX_PATH="$SATURN_SDL" "${CRT[@]}"
    quiet ninja -C $B/optest-sdl saturn
    # it starts, and answers a bad argument with its usage
    usage=$($B/optest-sdl/saturn --help 2>&1 || true)
    grep -q "usage: saturn" <<< "$usage"
    if [ -n "$WINDOWS" ]; then
        if llvm-objdump -p $B/optest-sdl/saturn.exe | grep -iE "DLL Name: (vcruntime|msvcp)"; then
            echo "saturn.exe needs Microsoft's C++ redistributable"
            exit 1
        fi
        video=$($B/optest-sdl/saturn --video out.mp4 2>&1 || true)
        grep -q -- "--video: not available on Windows" <<< "$video"
    fi
fi
