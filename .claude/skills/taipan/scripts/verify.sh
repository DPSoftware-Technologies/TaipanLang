#!/bin/sh
# Builds Taipan and runs every test suite. Exit code 0 = everything passed.
#   verify.sh              optimized build + all tests (default, tiny and stress GC)
#   verify.sh --warnings   also a -Wall -Wextra debug build that must be warning-free
#   verify.sh --quick      skip the slow GC stress run
set -u
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
cd "$ROOT" || exit 1
CXX="${CXX:-g++}"
EXE=""
case "$(uname -s 2>/dev/null)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac
PY="$(command -v python3 || command -v python)"
WARN=0
QUICK=0
for a in "$@"; do
    [ "$a" = "--warnings" ] && WARN=1
    [ "$a" = "--quick" ] && QUICK=1
done

failed=""
step() {
    name="$1"
    shift
    printf '== %s\n' "$name"
    if "$@"; then
        printf '   ok\n'
    else
        printf '   FAILED\n'
        failed="$failed\n  - $name"
    fi
}

if [ $WARN = 1 ]; then
    step "debug build is warning-free" sh -c \
        "$CXX -std=c++17 -O0 -Wall -Wextra -Iinclude src/*.cpp tools/taipan_main.cpp -o build/taipan_warncheck$EXE 2>&1 | tee /dev/stderr | grep -qE 'warning|error' && exit 1 || exit 0"
    rm -f "build/taipan_warncheck$EXE"
fi
step "build taipan + taipan-lsp" ./build.sh
step "build api_test" "$CXX" -std=c++17 -O2 -Iinclude src/*.cpp tests/api_test.cpp -o "build/api_test$EXE"
step "golden tests (default GC)" tests/run_tests.sh
step "golden tests (tiny incremental GC)" tests/run_tests.sh --gc-tiny
[ $QUICK = 0 ] && step "golden tests (GC stress)" tests/run_tests.sh --gc-stress
step "embedding API test" sh -c "cd tests && ../build/api_test$EXE"
if [ -n "$PY" ]; then
    step "language server test" "$PY" tests/lsp_test.py
else
    failed="$failed\n  - language server test (python not found)"
fi
step "benchmark results verified" sh -c "build/taipan$EXE --no-cache --no-warnings examples/benchmark.tp | grep -q 'results verified: true'"

if [ -n "$failed" ]; then
    printf '\nFAILED:%b\n' "$failed"
    exit 1
fi
printf '\nall checks passed\n'
