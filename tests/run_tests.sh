#!/bin/sh
# Runs every tests/<suite>/*.watr through watrc and compares stdout+stderr
# and the exit code against the matching .expected file.
#   tests/lexer/   watrc --tokens
#   tests/parser/  watrc --ast
#
#   UPDATE=1 ./tests/run_tests.sh   rewrite .expected files from current output
set -u
cd "$(dirname "$0")/.."

WATRC=${WATRC:-./build/watrc}
export NO_COLOR=1

pass=0
fail=0
for src in tests/lexer/*.watr tests/parser/*.watr; do
    case "$src" in
        tests/lexer/*)  flag=--tokens ;;
        tests/parser/*) flag=--ast ;;
    esac
    expected="${src%.watr}.expected"
    actual=$("$WATRC" "$flag" "$src" 2>&1; echo "exit: $?")

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
