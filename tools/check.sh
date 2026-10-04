#!/bin/bash
# Every check that needs no game data: the Python tests, the recompiler's instruction self-test and
# the runtime's unit tests, all built without SDL. CI runs this; so can you, before a commit.
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
