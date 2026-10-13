#!/usr/bin/env python3
"""Differential test for expressions over every scalar type.

Generates random watrlang expressions of every integer type, bool, f32 and
f64 (with casts between them), compiles them with watrc, runs the binary,
and compares each result with a Python model of watrlang's semantics:

  - integers wrap at their own width (two's complement)
  - division truncates toward zero; % takes the dividend's sign
  - shift counts are taken mod 64, then the result wraps to the type
  - f32 arithmetic rounds to single precision after every operation
  - float -> int truncates toward zero (only in-range values are generated)

main() checks each expression and returns the index of the first wrong
one, or 0.

    python3 tests/expr_fuzz.py [--seed N] [--programs N] [--per-program N]
"""
import argparse
import math
import os
import random
import struct
import subprocess
import sys
import tempfile

INTS = {
    "i8": (8, True), "i16": (16, True), "i32": (32, True), "i64": (64, True), "int": (64, True),
    "u8": (8, False), "u16": (16, False), "u32": (32, False), "u64": (64, False), "usize": (64, False),
}
FLOATS = ("f32", "f64")


class Undefined(Exception):
    """The expression traps, isn't representable, or is unspecified; skip it."""


def wrap(t, v):
    bits, signed = INTS[t]
    v &= (1 << bits) - 1
    if signed and v >> (bits - 1):
        v -= 1 << bits
    return v


def tmin(t):
    bits, signed = INTS[t]
    return -(1 << (bits - 1)) if signed else 0


def tmax(t):
    bits, signed = INTS[t]
    return (1 << (bits - 1)) - 1 if signed else (1 << bits) - 1


def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]


def fround(t, x):
    if math.isnan(x) or math.isinf(x):
        raise Undefined()
    r = f32(x) if t == "f32" else x
    if math.isinf(r):
        raise Undefined()
    return r


def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


def int_lit(t, v):
    """A literal of exactly type t."""
    if v < 0:
        if v == tmin(t) and INTS[t][0] == 64:
            src = "(-9223372036854775807 - 1)"
        else:
            src = f"(-{-v})"
    else:
        src = str(v)
    return src if t == "int" else f"({src} as {t})"


def float_lit(t, x):
    s = repr(x)
    if "e" not in s and "." not in s and "inf" not in s and "nan" not in s:
        s += ".0"
    if x < 0 or s.startswith("-"):
        s = f"(-{s.lstrip('-')})"
    return s if t == "f64" else f"({s} as f32)"


def str_lit(v):
    out = []
    for ch in v:
        out.append({"\\": "\\\\", '"': '\\"', "{": "\\{", "}": "\\}", "\n": "\\n", "\t": "\\t"}.get(ch, ch))
    return '"' + "".join(out) + '"'


class Gen:
    def __init__(self, rng):
        self.rng = rng

    def pick_int(self):
        return self.rng.choice(list(INTS))

    # ---- ints ---------------------------------------------------------------

    def int_expr(self, t, depth):
        rng = self.rng
        r = rng.random()
        if depth <= 0 or r < 0.2:
            v = rng.choice([0, 1, 2, 3, 7, tmax(t), tmin(t), tmax(t) // 2]) if rng.random() < 0.5 \
                else rng.randint(tmin(t), tmax(t))
            return int_lit(t, v), v
        if r < 0.3:
            # Cast from another type.
            kind = rng.random()
            if kind < 0.6:
                u = self.pick_int()
                s, v = self.int_expr(u, depth - 1)
                return f"({s} as {t})", wrap(t, v)
            if kind < 0.8:
                s, v = self.bool_expr(depth - 1)
                return f"({s} as {t})", int(v)
            u = rng.choice(FLOATS)
            s, x = self.float_expr(u, depth - 1)
            iv = math.trunc(x)
            if not (tmin(t) <= iv <= tmax(t)):
                raise Undefined()
            return f"({s} as {t})", iv
        if r < 0.38:
            op = rng.choice(["-", "~"] if INTS[t][1] else ["~"])
            s, v = self.int_expr(t, depth - 1)
            return f"({op}{s})", wrap(t, -v if op == "-" else ~v)
        if r < 0.45:
            c, cv = self.bool_expr(depth - 1)
            a, av = self.int_expr(t, depth - 1)
            b, bv = self.int_expr(t, depth - 1)
            return f"({c} ? {a} : {b})", av if cv else bv
        op = rng.choice(["+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"])
        a, av = self.int_expr(t, depth - 1)
        if op in ("<<", ">>"):
            u = self.pick_int()
            b, bv = self.int_expr(u, depth - 1)
            c = bv & 63
            return f"({a} {op} {b})", wrap(t, av << c) if op == "<<" else wrap(t, av >> c)
        b, bv = self.int_expr(t, depth - 1)
        if op in ("/", "%"):
            if bv == 0 or (INTS[t][0] == 64 and av == tmin(t) and bv == -1):
                raise Undefined()   # traps
            q = tdiv(av, bv)
            v = q if op == "/" else av - q * bv
        else:
            v = {"+": av + bv, "-": av - bv, "*": av * bv, "&": av & bv, "|": av | bv, "^": av ^ bv}[op]
        return f"({a} {op} {b})", wrap(t, v)

    # ---- floats -------------------------------------------------------------

    def float_expr(self, t, depth):
        rng = self.rng
        r = rng.random()
        if depth <= 0 or r < 0.25:
            x = rng.choice([0.0, 1.0, 0.5, 0.1, 2.5, 1e10, 3.14159, -7.25, 1e-5])
            if rng.random() < 0.5:
                x = rng.uniform(-1e6, 1e6)
            x = fround(t, x)
            return float_lit(t, x), x
        if r < 0.38:
            if rng.random() < 0.5:
                u = self.pick_int()
                s, v = self.int_expr(u, depth - 1)
                if t == "f32" and abs(v) >= 2 ** 53:
                    raise Undefined()   # avoid double rounding in the model
                return f"({s} as {t})", fround(t, float(v))
            u = rng.choice(FLOATS)
            s, x = self.float_expr(u, depth - 1)
            return f"({s} as {t})", fround(t, x)
        if r < 0.45:
            s, x = self.float_expr(t, depth - 1)
            return f"(-{s})", -x
        if r < 0.52:
            c, cv = self.bool_expr(depth - 1)
            a, ax = self.float_expr(t, depth - 1)
            b, bx = self.float_expr(t, depth - 1)
            return f"({c} ? {a} : {b})", ax if cv else bx
        op = rng.choice(["+", "-", "*", "/"])
        a, ax = self.float_expr(t, depth - 1)
        b, bx = self.float_expr(t, depth - 1)
        if op == "/" and bx == 0.0:
            raise Undefined()
        x = {"+": lambda: ax + bx, "-": lambda: ax - bx, "*": lambda: ax * bx, "/": lambda: ax / bx}[op]()
        return f"({a} {op} {b})", fround(t, x)

    # ---- strings ------------------------------------------------------------

    def str_expr(self, depth):
        rng = self.rng
        r = rng.random()
        if depth <= 0 or r < 0.3:
            v = "".join(rng.choice('ab yz09{}"\\\n\t') for _ in range(rng.randint(0, 6)))
            return str_lit(v), v
        if r < 0.55:
            a, av = self.str_expr(depth - 1)
            b, bv = self.str_expr(depth - 1)
            return f"({a} . {b})", av + bv
        if r < 0.7:
            a, av = self.str_expr(depth - 1)
            t = self.pick_int()
            b, bv = self.int_expr(t, depth - 1)
            return f"({a} . {b})", av + str(bv)
        if r < 0.78:
            a, av = self.str_expr(depth - 1)
            b, bv = self.bool_expr(depth - 1)
            return f"({a} . {b})", av + ("true" if bv else "false")
        if r < 0.9:
            # Interpolation with an integer expression inside.
            t = self.pick_int()
            b, bv = self.int_expr(t, depth - 1)
            pre = "".join(rng.choice("ab ") for _ in range(rng.randint(0, 3)))
            return f'"{pre}{{{b}}}!"', f"{pre}{bv}!"
        c, cv = self.bool_expr(depth - 1)
        a, av = self.str_expr(depth - 1)
        b, bv = self.str_expr(depth - 1)
        return f"({c} ? {a} : {b})", av if cv else bv

    # ---- bools --------------------------------------------------------------

    def bool_expr(self, depth):
        rng = self.rng
        r = rng.random()
        if depth <= 0 or r < 0.1:
            v = rng.random() < 0.5
            return ("true" if v else "false"), v
        if r < 0.55:
            op = rng.choice(["==", "!=", "<", ">", "<=", ">="])
            if rng.random() < 0.75:
                t = self.pick_int()
                a, av = self.int_expr(t, depth - 1)
                b, bv = self.int_expr(t, depth - 1)
            else:
                t = rng.choice(FLOATS)
                a, av = self.float_expr(t, depth - 1)
                b, bv = self.float_expr(t, depth - 1)
            v = {"==": av == bv, "!=": av != bv, "<": av < bv, ">": av > bv, "<=": av <= bv, ">=": av >= bv}[op]
            return f"({a} {op} {b})", v
        if r < 0.62:
            s, v = self.bool_expr(depth - 1)
            return f"(!{s})", not v
        if r < 0.68:
            op = rng.choice(["==", "!=", "<", ">", "<=", ">="])
            a, av = self.str_expr(depth - 1)
            b, bv = self.str_expr(depth - 1)
            ab, bb = av.encode(), bv.encode()
            v = {"==": ab == bb, "!=": ab != bb, "<": ab < bb, ">": ab > bb, "<=": ab <= bb, ">=": ab >= bb}[op]
            return f"({a} {op} {b})", v
        op = rng.choice(["&&", "||", "==", "!=", "&", "|", "^"])
        a, av = self.bool_expr(depth - 1)
        b, bv = self.bool_expr(depth - 1)
        v = {"&&": av and bv, "||": av or bv, "==": av == bv, "!=": av != bv,
             "&": av and bv, "|": av or bv, "^": av != bv}[op]
        return f"({a} {op} {b})", v


def make_check(rng):
    g = Gen(rng)
    while True:
        try:
            kind = rng.random()
            depth = rng.randint(1, 4)
            if kind < 0.6:
                t = g.pick_int()
                s, v = g.int_expr(t, depth)
                return t, s, int_lit(t, v)
            if kind < 0.8:
                t = rng.choice(FLOATS)
                s, x = g.float_expr(t, depth)
                return t, s, float_lit(t, x)
            if kind < 0.9:
                s, v = g.str_expr(depth)
                return "string", s, str_lit(v)
            s, v = g.bool_expr(depth)
            return "bool", s, "true" if v else "false"
        except (Undefined, OverflowError):
            continue


def program_source(checks):
    lines = ["function main(): int {"]
    for i, (t, src, expected) in enumerate(checks, 1):
        # Assign to a typed variable first, so the expression's type is
        # exactly what the model assumed.
        lines.append(f"    {t} v{i} = {src};")
        lines.append(f"    if (v{i} != {expected}) {{ return {i}; }}")
    lines.append("    return 0;")
    lines.append("}")
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--programs", type=int, default=40)
    ap.add_argument("--per-program", type=int, default=50)
    ap.add_argument("--watrc", default=os.path.join(os.path.dirname(__file__), "..", "build", "watrc"))
    ap.add_argument("--keep", help="directory to save failing programs in")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n in range(args.programs):
            checks = [make_check(rng) for _ in range(args.per_program)]
            src = program_source(checks)
            src_path = os.path.join(tmp, f"p{n}.watr")
            bin_path = os.path.join(tmp, f"p{n}")
            with open(src_path, "w") as f:
                f.write(src)
            c = subprocess.run([args.watrc, "-o", bin_path, src_path], capture_output=True, text=True)
            ok = c.returncode == 0
            if not ok:
                print(f"program {n}: compile failed\n{c.stderr[:3000]}")
            else:
                r = subprocess.run([bin_path])
                ok = r.returncode == 0
                if not ok:
                    if 0 < r.returncode <= len(checks):
                        t, s, expected = checks[r.returncode - 1]
                        print(f"program {n}: wrong result\n  {t} expr: {s}\n  expected: {expected}")
                    else:
                        print(f"program {n}: crashed (status {r.returncode})")
            if not ok:
                failures += 1
                if args.keep:
                    os.makedirs(args.keep, exist_ok=True)
                    with open(os.path.join(args.keep, f"expr_{args.seed}_{n}.watr"), "w") as f:
                        f.write(src)
    total = args.programs * args.per_program
    print(f"{total} expressions in {args.programs} programs: {failures} failing program(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
