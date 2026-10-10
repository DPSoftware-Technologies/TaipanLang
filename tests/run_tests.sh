#!/bin/sh
# Golden-output tests: runs every script and compares stdout+stderr with
# tests/expected/<name>.out (carriage returns are ignored, so Windows and
# Unix builds compare the same).
#   ./run_tests.sh              run with the default GC
#   ./run_tests.sh --gc-tiny    run with tiny incremental GC thresholds
#   ./run_tests.sh --update     rewrite the expected files
cd "$(dirname "$0")"
TAIPAN="${TAIPAN:-../build/taipan}"
[ -x "$TAIPAN" ] || TAIPAN="$TAIPAN.exe"
CR=$(printf '\r')
UPDATE=0
EXTRA=""
for a in "$@"; do
    if [ "$a" = "--update" ]; then UPDATE=1; else EXTRA="$EXTRA $a"; fi
done
pass=0
fail=0
for t in *.tp errors/*.tp imports/main.tp imports/cycle/a.tp; do
    name=$(echo "$t" | sed 's|/|_|g; s|\.tp$||')
    out=$( (cd "$(dirname "$t")" && "$OLDPWD/$TAIPAN" --no-cache $EXTRA "$(basename "$t")" 2>&1; echo "exit=$?") | tr -d "$CR")
    if [ $UPDATE = 1 ]; then
        printf '%s\n' "$out" > "expected/$name.out"
        continue
    fi
    if [ "$out" = "$(tr -d "$CR" < "expected/$name.out" 2>/dev/null)" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "FAIL: $t"
        printf '%s\n' "$out" | diff "expected/$name.out" - | head -20
    fi
done
[ $UPDATE = 1 ] && { echo "expected outputs updated"; exit 0; }
echo "$pass passed, $fail failed"
[ $fail = 0 ]
