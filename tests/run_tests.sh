#!/bin/sh
# Runs every tests/<suite>/*.watr through watrc and compares stdout+stderr
# and the exit code against the matching .expected file.
#   tests/lexer/   watrc --tokens
#   tests/parser/  watrc --ast
#   tests/run/     compile, run the binary, record its output and exit code
#
#   UPDATE=1 ./tests/run_tests.sh   rewrite .expected files from current output
set -u
cd "$(dirname "$0")/.."

WATRC=${WATRC:-./build/watrc}
export NO_COLOR=1

pass=0
fail=0
mkdir -p build/tests
for src in tests/lexer/*.watr tests/parser/*.watr tests/run/*.watr; do
    expected="${src%.watr}.expected"
    case "$src" in
        tests/lexer/*)  actual=$("$WATRC" --tokens "$src" 2>&1; echo "exit: $?") ;;
        tests/parser/*) actual=$("$WATRC" --ast "$src" 2>&1; echo "exit: $?") ;;
        tests/run/*)
            bin="build/tests/$(basename "${src%.watr}")"
            rm -f "$bin"
            if compile_out=$("$WATRC" -o "$bin" "$src" 2>&1); then
                actual=$("$bin" 2>&1; echo "exit: $?")
            else
                actual=$(printf '%s\ncompile failed: %s' "$compile_out" "$?")
            fi
            ;;
    esac

    if [ "${UPDATE:-0}" = 1 ]; then
        printf '%s\n' "$actual" > "$expected"
        echo "updated $expected"
        continue
    fi

    if [ ! -f "$expected" ]; then
        echo "FAIL $src (no .expected file; run with UPDATE=1)"
        fail=$((fail + 1))
    elif printf '%s\n' "$actual" | diff -u "$expected" - > /tmp/watr_diff.$$ 2>&1; then
        pass=$((pass + 1))
    else
        echo "FAIL $src"
        cat /tmp/watr_diff.$$
        fail=$((fail + 1))
    fi
done
rm -f /tmp/watr_diff.$$

[ "${UPDATE:-0}" = 1 ] && exit 0
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
