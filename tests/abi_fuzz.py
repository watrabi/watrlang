#!/usr/bin/env python3
"""Differential test of the C calling convention.

Generates random struct types (mixed integer and float fields of every
size, nested structs, small arrays) and checks that watrlang and gcc-
compiled C agree on layout and on how they're passed and returned by
value, in both directions:

  - C returns a struct, watrlang receives it and hashes the fields
  - watrlang returns a struct (exported), C receives it and hashes it
  - many struct, int and float arguments mixed, so some go in registers
    and some spill to the stack, called from each side

main() returns the index of the first disagreement, or 0.

    python3 tests/abi_fuzz.py [--seed N] [--programs N]
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

SCALARS = {
    "i8": ("int8_t", True), "u8": ("uint8_t", False), "i16": ("int16_t", True), "u16": ("uint16_t", False),
    "i32": ("int32_t", True), "u32": ("uint32_t", False), "i64": ("int64_t", True), "u64": ("uint64_t", False),
    "f32": ("float", True), "f64": ("double", True),
}


class Struct:
    def __init__(self, name, fields):
        self.name = name
        self.fields = fields   # (name, kind, arg) with kind "scalar"/"array"/"struct"


def gen_structs(rng, count):
    structs = []
    for k in range(count):
        fields = []
        for i in range(rng.randint(1, 5)):
            r = rng.random()
            if r < 0.15 and structs:
                fields.append((f"f{i}", "struct", rng.choice(structs)))
            elif r < 0.3:
                fields.append((f"f{i}", "array", (rng.choice(list(SCALARS)), rng.randint(1, 3))))
            else:
                fields.append((f"f{i}", "scalar", rng.choice(list(SCALARS))))
        structs.append(Struct(f"S{k}", fields))
    return structs


def leaves(s, path=""):
    """Every scalar inside a struct: (access path parts, scalar type)."""
    out = []
    for name, kind, arg in s.fields:
        if kind == "scalar":
            out.append(([name], arg))
        elif kind == "array":
            t, n = arg
            for i in range(n):
                out.append(([name, i], t))
        else:
            for parts, t in leaves(arg):
                out.append(([name] + parts, t))
    return out


def c_path(var, parts):
    s = var
    for p in parts:
        s += f"[{p}]" if isinstance(p, int) else f".{p}"
    return s


def w_path(var, parts):
    s = var
    for p in parts:
        s += f"[{p}]" if isinstance(p, int) else f"->{p}"
    return s


def c_source(structs):
    out = ["#include <stdint.h>", ""]
    for s in structs:
        out.append(f"typedef struct {s.name} {{")
        for name, kind, arg in s.fields:
            if kind == "scalar":
                out.append(f"    {SCALARS[arg][0]} {name};")
            elif kind == "array":
                out.append(f"    {SCALARS[arg[0]][0]} {name}[{arg[1]}];")
            else:
                out.append(f"    {arg.name} {name};")
        out.append(f"}} {s.name};")
    out.append("")
    for s in structs:
        n = s.name
        out.append(f"{n} c_make_{n}(int64_t seed) {{")
        out.append(f"    {n} r;")
        for i, (parts, t) in enumerate(leaves(s)):
            out.append(f"    {c_path('r', parts)} = ({SCALARS[t][0]})(((seed * {i + 3} + {i * 7}) % 97) - 40);")
        out.append("    return r;")
        out.append("}")
        out.append(f"uint64_t c_hash_{n}({n} s) {{")
        out.append("    uint64_t h = 7;")
        for parts, t in leaves(s):
            v = c_path("s", parts)
            conv = f"(uint64_t)(int64_t){v}" if SCALARS[t][1] else f"(uint64_t){v}"
            out.append(f"    h = h * 31 + {conv};")
        out.append("    return h;")
        out.append("}")
        out.append(f"uint64_t c_mix_{n}(int64_t a, {n} s1, double d, {n} s2, int32_t b, float f, {n} s3) {{")
        out.append(f"    return c_hash_{n}(s1) + c_hash_{n}(s2) * 3 + c_hash_{n}(s3) * 5 + (uint64_t)a"
                   f" + (uint64_t)(int64_t)d + (uint64_t)(int64_t)b + (uint64_t)(int64_t)f;")
        out.append("}")
        out.append(f"{n} wx_make_{n}(int64_t seed);")
        out.append(f"uint64_t wx_mix_{n}(int64_t a, {n} s1, double d, {n} s2, int32_t b, float f, {n} s3);")
        out.append(f"uint64_t c_check_make_{n}(int64_t seed) {{ return c_hash_{n}(wx_make_{n}(seed)); }}")
        out.append(f"uint64_t c_drive_{n}(int64_t seed) {{")
        out.append(f"    return wx_mix_{n}(seed, c_make_{n}(seed), 2.0, c_make_{n}(seed + 1), 5, 3.0f, c_make_{n}(seed + 2));")
        out.append("}")
    return "\n".join(out) + "\n"


def w_source(structs):
    out = []
    for s in structs:
        out.append(f"struct {s.name} {{")
        for name, kind, arg in s.fields:
            if kind == "scalar":
                out.append(f"    {arg} {name};")
            elif kind == "array":
                out.append(f"    {arg[0]}[{arg[1]}] {name};")
            else:
                out.append(f"    {arg.name} {name};")
        out.append("}")
    for s in structs:
        n = s.name
        out.append(f"extern function c_make_{n}(i64 seed): {n};")
        out.append(f"extern function c_hash_{n}({n} s): u64;")
        out.append(f"extern function c_mix_{n}(i64 a, {n} s1, f64 d, {n} s2, i32 b, f32 f, {n} s3): u64;")
        out.append(f"extern function c_check_make_{n}(i64 seed): u64;")
        out.append(f"extern function c_drive_{n}(i64 seed): u64;")
        out.append(f"function w_hash_{n}({n} s): u64 {{")
        out.append("    u64 h = 7;")
        for parts, t in leaves(s):
            v = w_path("s", parts)
            conv = f"(({v} as i64) as u64)" if SCALARS[t][1] else f"({v} as u64)"
            out.append(f"    h = h * 31 + {conv};")
        out.append("    return h;")
        out.append("}")
        out.append(f"export function wx_make_{n}(i64 seed): {n} {{")
        out.append(f"    {n} r;")
        for i, (parts, t) in enumerate(leaves(s)):
            out.append(f"    {w_path('r', parts)} = (((seed * {i + 3} + {i * 7}) % 97) - 40) as {t};")
        out.append("    return r;")
        out.append("}")
        out.append(f"export function wx_mix_{n}(i64 a, {n} s1, f64 d, {n} s2, i32 b, f32 f, {n} s3): u64 {{")
        out.append(f"    return w_hash_{n}(s1) + w_hash_{n}(s2) * 3 + w_hash_{n}(s3) * 5 + (a as u64)"
                   f" + ((d as i64) as u64) + ((b as i64) as u64) + ((f as i64) as u64);")
        out.append("}")
    out.append("function main(): int {")
    check = 0
    for s in structs:
        n = s.name
        for seed in (1, 5, 40):
            ref = f"c_hash_{n}(c_make_{n}({seed}))"
            for expr in (f"w_hash_{n}(c_make_{n}({seed}))", f"c_hash_{n}(wx_make_{n}({seed}))",
                         f"c_check_make_{n}({seed})", f"w_hash_{n}(wx_make_{n}({seed}))"):
                check += 1
                out.append(f"    if ({expr} != {ref}) {{ return {check}; }}")
            mix = (f"c_mix_{n}({seed}, c_make_{n}({seed}), 2.0, c_make_{n}({seed + 1}), 5, 3.0, "
                   f"c_make_{n}({seed + 2}))")
            for expr in (f"wx_mix_{n}({seed}, c_make_{n}({seed}), 2.0, c_make_{n}({seed + 1}), 5, 3.0, "
                         f"c_make_{n}({seed + 2}))", f"c_drive_{n}({seed})"):
                check += 1
                out.append(f"    if ({expr} != {mix}) {{ return {check}; }}")
    out.append("    return 0;")
    out.append("}")
    return "\n".join(out) + "\n", check


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--programs", type=int, default=15)
    ap.add_argument("--structs", type=int, default=6)
    ap.add_argument("--watrc", default=os.path.join(os.path.dirname(__file__), "..", "build", "watrc"))
    ap.add_argument("--keep", help="directory to save failing programs in")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n in range(args.programs):
            structs = gen_structs(rng, args.structs)
            wsrc, nchecks = w_source(structs)
            csrc = c_source(structs)
            wpath, cpath, bpath = (os.path.join(tmp, f"p{n}{ext}") for ext in (".watr", ".c", ""))
            with open(wpath, "w") as f:
                f.write(wsrc)
            with open(cpath, "w") as f:
                f.write(csrc)
            c = subprocess.run([args.watrc, "-o", bpath, wpath, cpath], capture_output=True, text=True)
            ok = c.returncode == 0
            if not ok:
                print(f"program {n}: compile failed\n{c.stderr[:3000]}")
            else:
                r = subprocess.run([bpath])
                ok = r.returncode == 0
                if not ok:
                    print(f"program {n}: check {r.returncode} of {nchecks} failed")
            if not ok:
                failures += 1
                if args.keep:
                    os.makedirs(args.keep, exist_ok=True)
                    for path, src in ((wpath, wsrc), (cpath, csrc)):
                        with open(os.path.join(args.keep, f"abi_{args.seed}_{n}{os.path.splitext(path)[1]}"), "w") as f:
                            f.write(src)
    print(f"{args.programs} programs ({args.programs * args.structs} struct types): {failures} failing")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
