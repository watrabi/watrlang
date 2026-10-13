#!/usr/bin/env python3
"""Differential test for integer codegen.

Generates random watrlang int expressions, compiles each with watrc, runs
the binary, and compares its exit code with a Python model of watrlang's
semantics (64-bit two's complement, C-style division, shift counts taken
mod 64). Many expressions are batched into one program: main() returns the
index of the first wrong result, or 0 if all are right.

    python3 tests/expr_fuzz.py [--seed N] [--programs N] [--per-program N]
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

MASK = (1 << 64) - 1


def wrap(v):
    v &= MASK
    return v - (1 << 64) if v >> 63 else v


class Undefined(Exception):
    """The expression divides by zero or overflows idiv; skip it."""


def tdiv(a, b):
    if b == 0 or (a == -(1 << 63) and b == -1):
        raise Undefined()
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


BINOPS = {
    "+": lambda a, b: wrap(a + b),
    "-": lambda a, b: wrap(a - b),
    "*": lambda a, b: wrap(a * b),
    "/": lambda a, b: wrap(tdiv(a, b)),
    "%": lambda a, b: wrap(a - tdiv(a, b) * b),
    "&": lambda a, b: a & b,
    "|": lambda a, b: a | b,
    "^": lambda a, b: a ^ b,
    "<<": lambda a, b: wrap(a << (b & 63)),
    ">>": lambda a, b: a >> (b & 63),
    "==": lambda a, b: int(a == b),
    "!=": lambda a, b: int(a != b),
    "<": lambda a, b: int(a < b),
    ">": lambda a, b: int(a > b),
    "<=": lambda a, b: int(a <= b),
    ">=": lambda a, b: int(a >= b),
    "&&": lambda a, b: int(bool(a) and bool(b)),
    "||": lambda a, b: int(bool(a) or bool(b)),
}
UNOPS = {
    "-": lambda a: wrap(-a),
    "~": lambda a: wrap(~a),
    "!": lambda a: int(a == 0),
}
INTERESTING = [0, 1, 2, 3, 7, 8, 63, 64, 65, 255, 256, 1000, 2**31 - 1, 2**31, 2**32, 2**62, 2**63 - 1]


def literal(rng):
    v = rng.choice(INTERESTING) if rng.random() < 0.6 else rng.randrange(0, 2**63)
    return str(v), v


def gen(rng, depth):
    """Returns (source, value). Every subexpression is parenthesized, so the
    generator doesn't depend on precedence (tests/run/precedence.watr does)."""
    if depth == 0 or rng.random() < 0.25:
        return literal(rng)
    r = rng.random()
    if r < 0.15:
        op = rng.choice(list(UNOPS))
        src, v = gen(rng, depth - 1)
        return f"({op}{src})", UNOPS[op](v)
    if r < 0.25:
        c, cv = gen(rng, depth - 1)
        a, av = gen(rng, depth - 1)
        b, bv = gen(rng, depth - 1)
        return f"({c} ? {a} : {b})", av if cv else bv
    op = rng.choice(list(BINOPS))
    a, av = gen(rng, depth - 1)
    if op in ("&&", "||"):
        # Short-circuit: the right side's value only matters if it runs.
        b, bv = gen(rng, depth - 1)
        if (op == "&&" and not av) or (op == "||" and av):
            return f"({a} {op} {b})", int(op == "||")
        return f"({a} {op} {b})", BINOPS[op](av, bv)
    b, bv = gen(rng, depth - 1)
    return f"({a} {op} {b})", BINOPS[op](av, bv)


def make_program(rng, count):
    checks = []
    while len(checks) < count:
        try:
            src, v = gen(rng, rng.randint(1, 5))
        except Undefined:
            continue
        checks.append((src, v))
    return checks


def program_source(checks):
    # No variables or if-statements yet (milestone 4), so the program is one
    # big ternary chain: the first failing check's index is the exit code.
    expr = "0"
    for i in range(len(checks), 0, -1):
        src, v = checks[i - 1]
        # Negative values are written as 0 - N - 1 so even -2^63 avoids an
        # out-of-range literal. Comparing in-program checks all 64 bits.
        expected = f"(0 - {-v - 1} - 1)" if v < 0 else str(v)
        expr = f"(({src}) != {expected} ? {i} : {expr})"
    return f"function main(): int {{\n    return {expr};\n}}\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--programs", type=int, default=40)
    ap.add_argument("--per-program", type=int, default=50)
    ap.add_argument("--watrc", default=os.path.join(os.path.dirname(__file__), "..", "build", "watrc"))
    args = ap.parse_args()

    rng = random.Random(args.seed)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n in range(args.programs):
            checks = make_program(rng, args.per_program)
            src_path = os.path.join(tmp, f"p{n}.watr")
            bin_path = os.path.join(tmp, f"p{n}")
            with open(src_path, "w") as f:
                f.write(program_source(checks))
            c = subprocess.run([args.watrc, "-o", bin_path, src_path], capture_output=True, text=True)
            if c.returncode != 0:
                print(f"program {n}: compile failed\n{c.stderr}")
                failures += 1
                continue
            r = subprocess.run([bin_path])
            if r.returncode != 0:
                failures += 1
                if r.returncode < 0 or r.returncode > len(checks):
                    print(f"program {n}: crashed (status {r.returncode})")
                else:
                    src, v = checks[r.returncode - 1]
                    print(f"program {n}: wrong result\n  expr:     {src}\n  expected: {v}")
    total = args.programs * args.per_program
    print(f"{total} expressions in {args.programs} programs: {failures} failing program(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
