#!/bin/sh
# Times each benchmark, best of $RUNS runs (default 5): bench/run.sh [watrc flags...]
cd "$(dirname "$0")"
RUNS=${RUNS:-5}
for src in *.watr; do
    bin="../build/bench_${src%.watr}"
    ../build/watrc "$@" -o "$bin" "$src" || exit 1
    best=""
    i=0
    while [ $i -lt "$RUNS" ]; do
        start=$(date +%s%N)
        out=$("$bin")
        end=$(date +%s%N)
        t=$(( (end - start) / 1000000 ))
        if [ -z "$best" ] || [ "$t" -lt "$best" ]; then best=$t; fi
        i=$((i + 1))
    done
    printf '%-12s %4d ms  %s\n' "${src%.watr}" "$best" "$out"
done
