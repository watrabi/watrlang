#!/usr/bin/env python3
"""Differential test for division and remainder by constants.

-O1 turns `x / 7` into a multiply by a magic reciprocal and shifts, and
`x / 8` into shifts. This compiles programs that divide many runtime
values (edge cases and random ones) by many constant divisors of every
integer type, and compares each quotient and remainder with Python's
model: truncation toward zero, % taking the dividend's sign, results
wrapped to the type.

    python3 tests/div_fuzz.py [--seed N] [--programs N]
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

TYPES = {"i64": (64, True), "u64": (64, False), "i32": (32, True), "u32": (32, False),
         "i16": (16, True), "u8": (8, False), "int": (64, True), "usize": (64, False)}


def wrap(t, v):
    bits, signed = TYPES[t]
    v &= (1 << bits) - 1
    if signed and v >> (bits - 1):
        v -= 1 << bits
    return v


def lit(t, v):
    bits, signed = TYPES[t]
    if signed and v == -(1 << (bits - 1)):
        return f"({v + 1} - 1)"
    return str(v)


def divisors(t, rng):
    bits, signed = TYPES[t]
    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
    out = {2, 3, 5, 6, 7, 10, 12, 60, 100, 255, 641, 1000, 4096, 65535, 1000000007}
    out |= {(1 << k) for k in range(1, bits - 1)} | {(1 << k) - 1 for k in range(2, bits)} | {(1 << k) + 1 for k in range(2, bits - 1)}
    out |= {rng.randrange(2, hi) for _ in range(8)} | {hi, hi - 1}
    if signed:
        out |= {-v for v in list(out)} | {lo, lo + 1}
        out.discard(-1)
    return sorted(v for v in out if lo <= v <= hi and v not in (0, 1))


def dividends(t, rng):
    bits, signed = TYPES[t]
    lo, hi = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
    out = {0, 1, 2, 3, 6, 7, 8, 9, 99, 100, 101, hi, hi - 1, lo, lo + 1}
    if signed:
        out |= {-1, -2, -3, -6, -7, -8, -9, -100}
    out |= {rng.randint(lo, hi) for _ in range(10)} | {rng.randint(-1000, 1000) for _ in range(6)}
    return sorted(v for v in out if lo <= v <= hi)


def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def make_program(rng):
    lines, expected = [], []
    body = []
    for t in TYPES:
        xs = dividends(t, rng)
        ds = divisors(t, rng)
        lines.append(f"{t}[{len(xs)}] xs_{t} = [{', '.join(lit(t, x) for x in xs)}];")
        body.append(f"    for (usize i = 0; i < {len(xs)}; i++) {{")
        body.append(f"        {t} x = xs_{t}[i];")
        for d in ds:
            body.append(f"        echo x / {lit(t, d)}, \" \", x % {lit(t, d)}, \"\\n\";")
        body.append("    }")
        for x in xs:
            for d in ds:
                if TYPES[t][1] and x == -(1 << (TYPES[t][0] - 1)) and d == -1:
                    continue
                q = tdiv(x, d)
                expected.append(f"{wrap(t, q)} {wrap(t, x - q * d)}")
    src = "\n".join(lines) + "\n\nfunction main(): int {\n" + "\n".join(body) + "\n    return 0;\n}\n"
    return src, expected


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--programs", type=int, default=3)
    args = ap.parse_args()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    watrc = os.path.join(root, "build", "watrc")
    rng = random.Random(args.seed)
    failing = 0
    checked = 0
    with tempfile.TemporaryDirectory() as tmp:
        for p in range(args.programs):
            src, expected = make_program(rng)
            path = os.path.join(tmp, f"div{p}.watr")
            with open(path, "w") as f:
                f.write(src)
            for opt in ("-O1", "-O0"):
                exe = os.path.join(tmp, f"div{p}{opt}")
                r = subprocess.run([watrc, opt, "-o", exe, path], capture_output=True, text=True)
                if r.returncode != 0:
                    print(r.stderr)
                    sys.exit(1)
                out = subprocess.run([exe], capture_output=True, text=True, timeout=60).stdout.splitlines()
                checked += len(expected)
                if out != expected:
                    failing += 1
                    for i, (a, b) in enumerate(zip(out, expected)):
                        if a != b:
                            print(f"program {p} {opt}: line {i + 1}: got {a}, expected {b}")
                            break
                    else:
                        print(f"program {p} {opt}: {len(out)} lines, expected {len(expected)}")
    print(f"{checked} divisions in {args.programs * 2} programs: {failing} failing")
    sys.exit(1 if failing else 0)


if __name__ == "__main__":
    main()
