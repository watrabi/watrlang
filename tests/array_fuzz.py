#!/usr/bin/env python3
"""Differential test for PHP-style arrays.

Generates random sequences of array operations (set with int and string
keys, append, remove, read, count, copy, iterate), runs them natively and
in a Python model of PHP's ordered-map semantics, and compares the output.

    python3 tests/array_fuzz.py [--seed N] [--programs N] [--ops N]
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile


class Model:
    """PHP arrays: insertion order; overwriting keeps the position;
    appending uses one more than the largest int key ever used."""

    def __init__(self):
        self.items = {}   # Python dicts keep insertion order
        self.next = 0

    def set(self, k, v):
        if k not in self.items and isinstance(k, int) and k >= self.next:
            self.next = k + 1
        self.items[k] = v

    def push(self, v):
        self.set(self.next, v)

    def remove(self, k):
        return self.items.pop(k, None) is not None


def key_src(k):
    return f'"{k}"' if isinstance(k, str) else str(k)


def key_text(k):
    return k if isinstance(k, str) else str(k)


def make_program(rng, nops):
    """Source plus the expected output."""
    arrays = [Model(), Model()]
    lines = ["import \"std/arrays\";", "", "function dump(array a): void {",
             "    echo len(a), \":\";", "    foreach (a as k => v) {", "        echo \" \", k, \"=\", v;",
             "    }", "    echo \"\\n\";", "}", "", "function main(): int {",
             "    array a0 = [];", "    array a1 = [];"]
    out = []

    def rand_key(a=None):
        if a is not None and a.items and rng.random() < 0.7:   # usually one that's there
            return rng.choice(list(a.items))
        if rng.random() < 0.5:
            return rng.randint(-3, 40)
        return f"k{rng.randint(0, 25)}"

    for _ in range(nops):
        i = rng.randrange(2)
        a = arrays[i]
        name = f"a{i}"
        op = rng.random()
        if op < 0.35:
            k, v = rand_key(), rng.randint(-1000, 1000)
            lines.append(f"    {name}[{key_src(k)}] = {v};")
            a.set(k, v)
        elif op < 0.55:
            v = rng.randint(-1000, 1000)
            lines.append(f"    {name}[] = {v};")
            a.push(v)
        elif op < 0.70:
            k = rand_key(a)
            lines.append(f"    echo remove({name}, {key_src(k)}), \"\\n\";")
            out.append("true" if a.remove(k) else "false")
        elif op < 0.85:
            k = rand_key(a)
            lines.append(f"    echo \"[\", {name}[{key_src(k)}], \"]\\n\";")
            v = a.items.get(k)
            out.append(f"[{'' if v is None else v}]")
        elif op < 0.90:
            lines.append(f"    dump({name});")
            out.append(f"{len(a.items)}:" + "".join(f" {key_text(k)}={v}" for k, v in a.items.items()))
        elif op < 0.93:
            # copy() rebuilds keys in order; its append position follows the keys it copied
            j = 1 - i
            lines.append(f"    a{j} = copy({name});")
            c = Model()
            for k, v in a.items.items():
                c.set(k, v)
            arrays[j] = c
        elif op < 0.97:
            # remove every other key, leaving holes that later growth squeezes out
            lines.append(f"    foreach (keys({name}) as i => k) {{ if ((i as int) % 2 == 0) {{ remove({name}, k); }} }}")
            for k in list(a.items)[0::2]:
                a.remove(k)
        else:
            lines.append(f"    echo len({name}), \"\\n\";")
            out.append(str(len(a.items)))
    for i in range(2):
        lines.append(f"    dump(a{i});")
        a = arrays[i]
        out.append(f"{len(a.items)}:" + "".join(f" {key_text(k)}={v}" for k, v in a.items.items()))
    lines += ["    return 0;", "}", ""]
    return "\n".join(lines), "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--programs", type=int, default=20)
    ap.add_argument("--ops", type=int, default=400)
    ap.add_argument("--watrc", default=os.path.join(os.path.dirname(__file__), "..", "build", "watrc"))
    ap.add_argument("--keep", help="directory to save failing programs in")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        for n in range(args.programs):
            src, expected = make_program(rng, args.ops)
            src_path = os.path.join(tmp, f"a{n}.watr")
            bin_path = os.path.join(tmp, f"a{n}")
            with open(src_path, "w") as f:
                f.write(src)
            c = subprocess.run([args.watrc, "-o", bin_path, src_path], capture_output=True, text=True)
            ok = c.returncode == 0
            if not ok:
                print(f"program {n}: compile failed\n{c.stderr[:2000]}")
            else:
                try:
                    r = subprocess.run([bin_path], capture_output=True, text=True, timeout=20)
                except subprocess.TimeoutExpired:
                    r = None
                if r is None:
                    print(f"program {n}: timed out")
                    ok = False
                elif r.returncode != 0 or r.stdout != expected:
                    ok = False
                    got, want = r.stdout.splitlines(), expected.splitlines()
                    line = next((i for i in range(max(len(got), len(want)))
                                 if i >= len(got) or i >= len(want) or got[i] != want[i]), 0)
                    print(f"program {n}: output line {line + 1} differs (status {r.returncode})\n"
                          f"  got:  {got[line] if line < len(got) else '<missing>'}\n"
                          f"  want: {want[line] if line < len(want) else '<missing>'}")
            if not ok:
                failures += 1
                if args.keep:
                    os.makedirs(args.keep, exist_ok=True)
                    with open(os.path.join(args.keep, f"fail_{args.seed}_{n}.watr"), "w") as f:
                        f.write(src)
    print(f"{args.programs} programs ({args.programs * args.ops} array operations): {failures} failing")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
