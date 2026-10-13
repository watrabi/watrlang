#!/bin/sh
# Times each benchmark: bench/run.sh [watrc flags...]
cd "$(dirname "$0")"
for src in *.watr; do
    bin="../build/bench_${src%.watr}"
    ../build/watrc "$@" -o "$bin" "$src" || exit 1
    start=$(date +%s.%N)
    out=$("$bin")
    end=$(date +%s.%N)
    printf '%-12s %6.2fs  %s\n' "${src%.watr}" "$(echo "$end - $start" | bc)" "$out"
done
