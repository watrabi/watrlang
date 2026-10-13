#!/bin/sh
# Runs every tests/<suite>/*.watr through watrc and compares stdout+stderr
# and the exit code against the matching .expected file.
#   tests/lexer/   watrc --tokens
#   tests/parser/  watrc --ast
#   tests/run/     compile, run the binary, record its output and exit code
#
#   UPDATE=1 ./tests/run_tests.sh   rewrite .expected files from current output
#   WATR_GC_STRESS=1 ./tests/run_tests.sh   compiled programs collect garbage at every allocation
set -u
cd "$(dirname "$0")/.."

WATRC=${WATRC:-./build/watrc}
export NO_COLOR=1

pass=0
fail=0
mkdir -p build/tests
for src in tests/lexer/*.watr tests/parser/*.watr tests/run/*.watr; do
    expected="${src%.watr}.expected"
    # WATR_GC_STRESS=1 collects at every allocation; some tests are too big for that.
    if [ "${WATR_GC_STRESS:-0}" != 0 ] && grep -q 'no-gc-stress' "$src"; then
        continue
    fi
    case "$src" in
        tests/lexer/*)  actual=$("$WATRC" --tokens "$src" 2>&1; echo "exit: $?") ;;
        tests/parser/*) actual=$("$WATRC" --ast "$src" 2>&1; echo "exit: $?") ;;
        tests/run/*)
            bin="build/tests/$(basename "${src%.watr}")"
            rm -f "$bin"
            extra=""
            [ -f "${src%.watr}.c" ] && extra="${src%.watr}.c"   # C code to link in
            [ -f "${src%.watr}.s" ] && extra="$extra ${src%.watr}.s"   # and assembly
            flags=$(sed -n 's|^// watrc-flags: ||p' "$src")      # e.g. --freestanding
            if compile_out=$("$WATRC" $flags -o "$bin" "$src" $extra 2>&1); then
                run_out=$(timeout 30 "$bin" 2>&1; echo "exit: $?")
                # Keep warnings from a successful compile in the expected output.
                if [ -n "$compile_out" ]; then
                    actual=$(printf '%s\n%s' "$compile_out" "$run_out")
                else
                    actual=$run_out
                fi
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

# Every error code the compiler can emit must have an --explain entry.
for code in $(grep -ohE '"[EW][0-9]{4}"' src/*.c | tr -d '"' | sort -u); do
    if ! "$WATRC" --explain "$code" > /dev/null 2>&1; then
        echo "FAIL: $code has no --explain text (add it to src/explain.c)"
        fail=$((fail + 1))
    fi
done

[ "${UPDATE:-0}" = 1 ] && exit 0
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
