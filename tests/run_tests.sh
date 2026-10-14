#!/bin/sh
# Runs every tests/<suite>/*.watr through watrc and compares stdout+stderr
# and the exit code against the matching .expected file.
#   tests/lexer/   watrc --tokens
#   tests/parser/  watrc --ast
#   tests/run/     compile, run the binary, record its output and exit code
#
#   UPDATE=1 ./tests/run_tests.sh   rewrite .expected files from current output
#   WATR_GC_STRESS=1 ./tests/run_tests.sh   compiled programs collect garbage at every allocation
#   WATR_TARGET=windows ./tests/run_tests.sh   tests/run/ built for Windows, run with Wine
#       (tests marked `// targets: linux` are skipped; WINE picks the wine binary)
#   WATR_BUILD_ONLY=1 / WATR_PREBUILT=1   compile tests/run/ on one machine, run the
#       binaries on another (CI builds Windows .exe files on Linux, runs them on Windows)
set -u
cd "$(dirname "$0")/.."

WATRC=${WATRC:-./build/watrc}
export NO_COLOR=1

pass=0
fail=0
skip=0
suites="tests/lexer/*.watr tests/parser/*.watr tests/run/*.watr"
target_flags=""
runner=""
exe=""
if [ -n "${WATR_TARGET:-}" ] && [ "$WATR_TARGET" != linux ] && [ "$WATR_TARGET" != x86_64-linux ]; then
    # Another OS: only tests/run/ means anything (the front end doesn't change).
    suites="tests/run/*.watr"
    target_flags="--target $WATR_TARGET"
    case "$WATR_TARGET" in
        *windows*)
            exe=.exe
            case "$(uname -s)" in
                MINGW*|MSYS*|CYGWIN*) ;;   # on Windows itself: run the .exe files directly
                *) export WINEDEBUG=-all
                   runner=${WINE:-$(command -v wine64 || command -v wine || echo /usr/lib/wine/wine64)} ;;
            esac ;;
    esac
fi
mkdir -p build/tests
for src in $suites; do
    expected="${src%.watr}.expected"
    # WATR_GC_STRESS=1 collects at every allocation; some tests are too big for that.
    if [ "${WATR_GC_STRESS:-0}" != 0 ] && grep -q 'no-gc-stress' "$src"; then
        continue
    fi
    # `// targets: linux` (a list) limits a test to some OSes: inline asm
    # making system calls, --freestanding, assembly files...
    if [ -n "$target_flags" ]; then
        targets=$(sed -n 's|^// targets: ||p' "$src")
        if [ -n "$targets" ] && ! printf '%s\n' "$targets" | grep -qw "${WATR_TARGET#x86_64-}"; then
            skip=$((skip + 1))
            continue
        fi
    fi
    case "$src" in
        tests/lexer/*)  actual=$("$WATRC" --tokens "$src" 2>&1; echo "exit: $?") ;;
        tests/parser/*) actual=$("$WATRC" --ast "$src" 2>&1; echo "exit: $?") ;;
        tests/run/*)
            bin="build/tests/$(basename "${src%.watr}")$exe"
            [ "${WATR_PREBUILT:-0}" = 1 ] || rm -f "$bin"
            extra=""
            [ -f "${src%.watr}.c" ] && extra="${src%.watr}.c"   # C code to link in
            [ -f "${src%.watr}.s" ] && extra="$extra ${src%.watr}.s"   # and assembly
            flags=$(sed -n 's|^// watrc-flags: ||p' "$src")      # e.g. --freestanding
            if [ "${WATR_PREBUILT:-0}" = 1 ]; then
                compile_out=$(cat "$bin.compile")
                status=$(cat "$bin.status")
            else
                compile_out=$("$WATRC" $target_flags $flags -o "$bin" "$src" $extra 2>&1)
                status=$?
            fi
            if [ "${WATR_BUILD_ONLY:-0}" = 1 ]; then
                printf '%s' "$compile_out" > "$bin.compile"
                echo "$status" > "$bin.status"
                pass=$((pass + 1))
                continue
            fi
            if [ "$status" = 0 ]; then
                run_out=$(timeout 60 $runner "$bin" 2>&1; echo "exit: $?")
                # Keep warnings from a successful compile in the expected output.
                if [ -n "$compile_out" ]; then
                    actual=$(printf '%s\n%s' "$compile_out" "$run_out")
                else
                    actual=$run_out
                fi
            else
                actual=$(printf '%s\ncompile failed: %s' "$compile_out" "$status")
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

if [ "${WATR_BUILD_ONLY:-0}" = 1 ]; then
    echo "built $pass tests in build/tests (run them with WATR_PREBUILT=1)"
    exit 0
fi

# Every error code the compiler can emit must have an --explain entry.
[ "${WATR_PREBUILT:-0}" = 1 ] || for code in $(grep -ohE '"[EW][0-9]{4}"' src/*.c | tr -d '"' | sort -u); do
    if ! "$WATRC" --explain "$code" > /dev/null 2>&1; then
        echo "FAIL: $code has no --explain text (add it to src/explain.c)"
        fail=$((fail + 1))
    fi
done

[ "${UPDATE:-0}" = 1 ] && exit 0
if [ "$skip" -gt 0 ]; then
    echo "$pass passed, $fail failed, $skip skipped (not for this target)"
else
    echo "$pass passed, $fail failed"
fi
[ "$fail" -eq 0 ]
