#!/usr/bin/env python3
"""Differential test for statements, variables and calls.

Generates random watrlang programs (locals, assignments, if/else, bounded
loops with break/continue, calls with up to 8 arguments), runs each one
natively and in a Python model of watrlang, and compares the results.
main() checks every function's result itself and returns the index of
the first mismatch, or 0.

    python3 tests/prog_fuzz.py [--seed N] [--programs N]
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


def tdiv(a, b):
    q = abs(a) // abs(b)
    return q if (a < 0) == (b < 0) else -q


BIN = {
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
}
COMPOUND = ["+", "-", "*", "&", "|", "^", "<<", ">>", "/", "%"]
ARITH = ["+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>"]


class Break(Exception):
    pass


class Continue(Exception):
    pass


class Return(Exception):
    def __init__(self, value):
        self.value = value


def lit(v):
    """A watrlang literal for any int64, even negative ones."""
    return f"(0 - {-v - 1} - 1)" if v < 0 else str(v)


# ---- expressions: (source, evaluator(env, funcs) -> int) ---------------------

class Gen:
    def __init__(self, rng):
        self.rng = rng
        self.funcs = []      # (name, nparams, body, params, ret)
        self.counter = 0

    def fresh(self, prefix):
        self.counter += 1
        return f"{prefix}_{self.counter}"

    def expr(self, scope, depth):
        """An int expression: (source, evaluator(env, funcs) -> int)."""
        rng = self.rng
        r = rng.random()
        if depth <= 0 or r < 0.3:
            if scope and rng.random() < 0.6:
                name = rng.choice(scope)
                return name, lambda env, f, n=name: env[n]
            v = rng.choice([0, 1, 2, 3, 5, 7, 10, 100, -1, -8, 2**40, rng.randrange(-1000, 1000)])
            return lit(v), lambda env, f, v=v: v
        if r < 0.4 and self.funcs:
            name, nparams = rng.choice([(fn[0], fn[1]) for fn in self.funcs])
            args = [self.expr(scope, depth - 1) for _ in range(nparams)]
            src = f"{name}({', '.join(a[0] for a in args)})"
            return src, lambda env, f, n=name, args=args: call(f, n, [a[1](env, f) for a in args])
        if r < 0.48:
            op = rng.choice(["-", "~"])
            s, e = self.expr(scope, depth - 1)
            fn = {"-": lambda a: wrap(-a), "~": lambda a: wrap(~a)}[op]
            return f"({op}{s})", lambda env, f, e=e, fn=fn: fn(e(env, f))
        if r < 0.56:
            c, ce = self.cond(scope, depth - 1)
            a, ae = self.expr(scope, depth - 1)
            b, be = self.expr(scope, depth - 1)
            return f"({c} ? {a} : {b})", lambda env, f: ae(env, f) if ce(env, f) else be(env, f)
        if r < 0.62:
            c, ce = self.cond(scope, depth - 1)
            return f"({c} as int)", lambda env, f, ce=ce: int(ce(env, f))
        op = rng.choice(list(ARITH))
        a, ae = self.expr(scope, depth - 1)
        if op in ("/", "%"):
            # A divisor that can't be 0 or -1, so the model never traps.
            d = rng.choice([1, 2, 3, 7, 10, -2, -5])
            return f"({a} {op} {lit(d)})", lambda env, f, ae=ae, d=d, op=op: BIN[op](ae(env, f), d)
        b, be = self.expr(scope, depth - 1)
        return f"({a} {op} {b})", lambda env, f, ae=ae, be=be, op=op: BIN[op](ae(env, f), be(env, f))

    def cond(self, scope, depth):
        """A bool expression: (source, evaluator(env, funcs) -> bool)."""
        rng = self.rng
        r = rng.random()
        if depth <= 0 or r < 0.5:
            op = rng.choice(["==", "!=", "<", ">", "<=", ">="])
            a, ae = self.expr(scope, max(depth - 1, 0))
            b, be = self.expr(scope, max(depth - 1, 0))
            return f"({a} {op} {b})", lambda env, f, ae=ae, be=be, op=op: bool(BIN[op](ae(env, f), be(env, f)))
        if r < 0.6:
            s, e = self.cond(scope, depth - 1)
            return f"(!{s})", lambda env, f, e=e: not e(env, f)
        if r < 0.65:
            v = rng.random() < 0.5
            return ("true" if v else "false"), lambda env, f, v=v: v
        op = rng.choice(["&&", "||"])
        a, ae = self.cond(scope, depth - 1)
        b, be = self.cond(scope, depth - 1)
        if op == "&&":
            return f"({a} && {b})", lambda env, f: ae(env, f) and be(env, f)
        return f"({a} || {b})", lambda env, f: ae(env, f) or be(env, f)

    # ---- statements: (source lines, executor(env, funcs)) --------------------

    def block(self, scope, depth, in_loop, readonly, indent):
        """A list of statements. Variables declared here go out of scope at
        the end of the block, like in watrlang."""
        scope = list(scope)
        stmts = []
        for _ in range(self.rng.randint(1, 4)):
            stmts.append(self.stmt(scope, depth, in_loop, readonly, indent))
        lines = [l for s in stmts for l in s[0]]

        def run(env, f, stmts=stmts):
            for s in stmts:
                s[1](env, f)
        return lines, run

    def stmt(self, scope, depth, in_loop, readonly, indent):
        rng = self.rng
        pad = "    " * indent
        writable = [v for v in scope if v not in readonly]
        r = rng.random()

        if r < 0.25 or not writable:
            name = self.fresh("v")
            s, e = self.expr(scope, 2)
            scope.append(name)

            def run(env, f, name=name, e=e):
                env[name] = e(env, f)
            return [f"{pad}int {name} = {s};"], run

        if r < 0.45:
            name = rng.choice(writable)
            op = rng.choice(["="] + COMPOUND)
            if op in ("/", "%"):
                d = rng.choice([1, 3, 7, -2])
                s, e = lit(d), (lambda env, f, d=d: d)
            else:
                s, e = self.expr(scope, 2)
            if op == "=":
                def run(env, f, name=name, e=e):
                    env[name] = e(env, f)
            else:
                def run(env, f, name=name, e=e, op=op):
                    env[name] = BIN[op](env[name], e(env, f))
            return [f"{pad}{name} {op if op == '=' else op + '='} {s};"], run

        if r < 0.52:
            name = rng.choice(writable)
            op = rng.choice(["++", "--"])
            delta = 1 if op == "++" else -1

            def run(env, f, name=name, delta=delta):
                env[name] = wrap(env[name] + delta)
            return [f"{pad}{name}{op};"], run

        if r < 0.64 and depth > 0:
            c, ce = self.cond(scope, 2)
            tl, tr = self.block(scope, depth - 1, in_loop, readonly, indent + 1)
            el, er = self.block(scope, depth - 1, in_loop, readonly, indent + 1)
            lines = [f"{pad}if ({c}) {{"] + tl + [f"{pad}}} else {{"] + el + [f"{pad}}}"]

            def run(env, f, ce=ce, tr=tr, er=er):
                if ce(env, f):
                    tr(env, f)
                else:
                    er(env, f)
            return lines, run

        if r < 0.74 and depth > 0:
            i = self.fresh("i")
            n = rng.randint(0, 4)
            body_l, body_r = self.block(scope + [i], depth - 1, True, readonly | {i}, indent + 1)
            lines = [f"{pad}for (int {i} = 0; {i} < {n}; {i}++) {{"] + body_l + [f"{pad}}}"]

            def run(env, f, i=i, n=n, body_r=body_r):
                env[i] = 0
                while env[i] < n:
                    try:
                        body_r(env, f)
                    except Continue:
                        pass
                    except Break:
                        break
                    env[i] += 1
            return lines, run

        if r < 0.82 and depth > 0:
            w = self.fresh("w")
            n = rng.randint(0, 4)
            body_l, body_r = self.block(scope + [w], depth - 1, True, readonly | {w}, indent + 1)
            lines = ([f"{pad}int {w} = {n};", f"{pad}while ({w} > 0) {{", f"{pad}    {w}--;"]
                     + body_l + [f"{pad}}}"])
            scope.append(w)

            def run(env, f, w=w, n=n, body_r=body_r):
                env[w] = n
                while env[w] > 0:
                    env[w] -= 1
                    try:
                        body_r(env, f)
                    except Continue:
                        pass
                    except Break:
                        break
            return lines, run

        if r < 0.9 and in_loop:
            c, ce = self.cond(scope, 1)
            kind = rng.choice(["break", "continue"])
            exc = Break if kind == "break" else Continue

            def run(env, f, ce=ce, exc=exc):
                if ce(env, f):
                    raise exc()
            return [f"{pad}if ({c}) {kind};"], run

        if r < 0.93:
            # Early returns hand back a checksum of everything in scope, so
            # every variable's value shows up in the function's result.
            c, ce = self.cond(scope, 2)
            hs, he = checksum(scope)

            def run(env, f, ce=ce, he=he):
                if ce(env, f):
                    raise Return(he(env))
            return [f"{pad}if ({c}) return {hs};"], run

        # A call for its side effects only (functions here have none, but
        # it exercises call alignment in statement position).
        s, e = self.expr(scope, 1)

        def run(env, f, e=e):
            e(env, f)
        return [f"{pad}{s};"], run

    def function(self):
        name = self.fresh("f")
        nparams = self.rng.choice([0, 1, 2, 3, 6, 7, 8])
        params = [self.fresh("p") for _ in range(nparams)]
        body_l, body_r = self.block(params, 2, False, frozenset(), 1)
        ret_s, ret_e = self.expr(params, 2)
        hs, he = checksum(params)
        src = (f"function {name}({', '.join('int ' + p for p in params)}): int {{\n"
               + "\n".join(body_l) + f"\n    return {ret_s} + {hs};\n}}\n")

        def run(f, args, params=params, body_r=body_r, ret_e=ret_e, he=he):
            env = dict(zip(params, args))
            try:
                body_r(env, f)
                return wrap(ret_e(env, f) + he(env))
            except Return as r:
                return r.value
        self.funcs.append((name, nparams))
        return name, nparams, src, run


def checksum(scope):
    """((v1 * 31 + v2) * 31 + v3)... over the variables in scope."""
    src = "0"
    for v in scope:
        src = f"({src} * 31 + {v})"

    def ev(env, scope=list(scope)):
        h = 0
        for v in scope:
            h = wrap(h * 31 + env[v])
        return h
    return src, ev


def call(funcs, name, args):
    return funcs[name](funcs, args)


def make_program(rng, nfuncs):
    g = Gen(rng)
    sources, runners, sigs = [], {}, []
    for _ in range(nfuncs):
        name, nparams, src, run = g.function()
        sources.append(src)
        runners[name] = run
        sigs.append((name, nparams))

    checks = []
    for name, nparams in sigs:
        args = [rng.randrange(-50, 50) for _ in range(nparams)]
        expected = runners[name](runners, args)
        checks.append((f"{name}({', '.join(lit(a) for a in args)})", expected))

    main = ["function main(): int {"]
    for i, (call_src, expected) in enumerate(checks, 1):
        main.append(f"    if ({call_src} != {lit(expected)}) return {i};")
    main.append("    return 0;\n}\n")
    return "\n".join(sources) + "\n" + "\n".join(main), checks


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--programs", type=int, default=30)
    ap.add_argument("--funcs", type=int, default=6)
    ap.add_argument("--watrc", default=os.path.join(os.path.dirname(__file__), "..", "build", "watrc"))
    ap.add_argument("--keep", help="directory to save failing programs in")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n in range(args.programs):
            src, checks = make_program(rng, args.funcs)
            src_path = os.path.join(tmp, f"p{n}.watr")
            bin_path = os.path.join(tmp, f"p{n}")
            with open(src_path, "w") as f:
                f.write(src)
            c = subprocess.run([args.watrc, "-o", bin_path, src_path], capture_output=True, text=True)
            ok = c.returncode == 0
            if not ok:
                print(f"program {n}: compile failed\n{c.stderr[:2000]}")
            else:
                try:
                    r = subprocess.run([bin_path], timeout=20)
                except subprocess.TimeoutExpired:
                    r = None
                ok = r is not None and r.returncode == 0
                if r is None:
                    print(f"program {n}: timed out (infinite loop?)")
                elif not ok:
                    what = (f"check {r.returncode} ({checks[r.returncode - 1][0]}) is wrong, expected "
                            f"{checks[r.returncode - 1][1]}") if 0 < r.returncode <= len(checks) \
                        else f"crashed (status {r.returncode})"
                    print(f"program {n}: {what}")
            if not ok:
                failures += 1
                if args.keep:
                    os.makedirs(args.keep, exist_ok=True)
                    with open(os.path.join(args.keep, f"fail_{args.seed}_{n}.watr"), "w") as f:
                        f.write(src)
    print(f"{args.programs} programs ({args.programs * args.funcs} functions): {failures} failing")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
