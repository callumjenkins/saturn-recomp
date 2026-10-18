#!/bin/bash
# Every check that needs no game data: the Python tests, the recompiler's instruction self-test and
# the runtime's unit tests, all built without SDL. CI runs this; so can you, before a commit.
# With SATURN_SDL set to an SDL3 install prefix, it also builds the self-test's program on the SDL host.
set -euo pipefail
cd "$(dirname "$0")/.."
B=build/check
CMAKE=(-G Ninja -DCMAKE_CXX_COMPILER="${CXX:-clang++}" -DSATURN_NO_SDL=ON)

echo "== Python tests"
uv run pytest -q

echo "== instruction self-test"
uv run python -m saturnrecomp.recomp --out $B/optest --optest > /dev/null
cmake -S $B/optest -B $B/optest-build "${CMAKE[@]}" > /dev/null
ninja -C $B/optest-build selftest saturn > /dev/null
$B/optest-build/selftest $B/optest/selftest/optest.txt

echo "== runtime unit tests"
cmake -S runtime/tests -B $B/runtime-tests "${CMAKE[@]}" > /dev/null
ninja -C $B/runtime-tests > /dev/null
$B/runtime-tests/runtime_tests

echo "== runtime unit tests, VDP1 by Mednafen's rules (SATURN_VDP1_GPL)"
cmake -S runtime/tests -B $B/runtime-tests-gpl "${CMAKE[@]}" -DSATURN_VDP1_GPL=ON > /dev/null
ninja -C $B/runtime-tests-gpl > /dev/null
$B/runtime-tests-gpl/runtime_tests

if [ -n "${SATURN_SDL:-}" ]; then
    echo "== the SDL host"
    cmake -S $B/optest -B $B/optest-sdl -G Ninja -DCMAKE_CXX_COMPILER="${CXX:-clang++}" -DSATURN_REQUIRE_SDL=ON \
        -DCMAKE_PREFIX_PATH="$SATURN_SDL" > /dev/null
    ninja -C $B/optest-sdl saturn > /dev/null
    $B/optest-sdl/saturn --help 2>&1 | grep -q "usage: saturn"     # it starts, and answers a bad argument
fi
