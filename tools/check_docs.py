#!/usr/bin/env python3
"""Compiles every ```watr code block in the documentation, so examples stay true.

  - a block with `function main(` is a whole program: it must compile and
    exit with status 0 (or `// expect: exit N` on its first line);
  - `// expect: error E0xxx` on the first line: it must fail with that code;
  - `// fragment` on the first line: not checked (it leaves things out);
  - other blocks are fragments: they must compile either as top-level
    declarations or as the body of main (imports go to the top);
  - `// watrc-flags: ...` passes flags (e.g. --freestanding -c).

    python3 tools/check_docs.py [files...]   (default: docs/LANGUAGE.md and the skill)
"""
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WATRC = os.path.join(ROOT, "build", "watrc")


def blocks(path):
    text = open(path).read()
    for m in re.finditer(r"^```watr\n(.*?)^```", text, re.S | re.M):
        line = text.count("\n", 0, m.start()) + 1
        yield line, m.group(1)


def compile_src(src, tmp, flags, out_name="prog"):
    path = os.path.join(tmp, "doc.watr")
    with open(path, "w") as f:
        f.write(src)
    exe = os.path.join(tmp, out_name)
    if os.path.exists(exe):
        os.remove(exe)
    r = subprocess.run([WATRC] + flags + ["-o", exe, path], capture_output=True, text=True)
    return r, exe


def check(src, tmp):
    first = src.split("\n", 1)[0].strip()
    if first == "// fragment":
        return None
    flags = []
    m = re.search(r"^// watrc-flags: (.*)$", src, re.M)
    if m:
        flags = m.group(1).split()
    if first.startswith("// expect: error"):
        code = first.split()[-1]
        r, _ = compile_src(src, tmp, flags)
        if r.returncode == 0:
            return f"expected error {code}, but it compiled"
        if f"[{code}]" not in r.stderr:
            return f"expected error {code}, got:\n{r.stderr[:1500]}"
        return None
    if "function main(" in src or "--freestanding" in " ".join(flags):
        want = 0
        if first.startswith("// expect: exit"):
            want = int(first.split()[-1])
        r, exe = compile_src(src, tmp, flags)
        if r.returncode != 0:
            return "doesn't compile:\n" + r.stderr[:2000]
        if "-c" in flags or "--freestanding" in flags:
            return None
        try:
            run = subprocess.run([exe], capture_output=True, text=True, timeout=20, cwd=tmp)
        except subprocess.TimeoutExpired:
            return "timed out"
        if run.returncode != want:
            return f"exited with {run.returncode}, expected {want}:\n{run.stdout[-500:]}{run.stderr[-500:]}"
        return None
    # A fragment: as declarations, or as statements in main.
    imports = [l for l in src.split("\n") if l.startswith("import ")]
    rest = "\n".join(l for l in src.split("\n") if not l.startswith("import "))
    attempts = [
        src + "\nfunction main(): int { return 0; }\n",
        "\n".join(imports) + "\nfunction main(): int {\n" + rest + "\n    return 0;\n}\n",
        "\n".join(imports) + "\nfunction main(): int {\n" + rest + "\n}\n",
    ]
    errors = []
    for a in attempts:
        r, _ = compile_src(a, tmp, flags)
        if r.returncode == 0:
            return None
        errors.append(r.stderr)
    return "fragment doesn't compile (as declarations or in main):\n" + min(errors, key=len)[:2000]


def stale_copies():
    """The skill ships copies of the guides; they must match docs/ (`make docs` syncs them)."""
    out = []
    for name in ("LANGUAGE.md", "STDLIB.md"):
        src = os.path.join(ROOT, "docs", name)
        dst = os.path.join(ROOT, "skills", "watrlang", "references", name)
        if not os.path.exists(dst) or open(src).read() != open(dst).read():
            out.append(f"skills/watrlang/references/{name} is out of date: run `make docs`")
    return out


def main():
    for msg in stale_copies():
        print(msg)
        sys.exit(1)
    files = sys.argv[1:] or [os.path.join(ROOT, "docs", "LANGUAGE.md")] + \
        sorted(glob.glob(os.path.join(ROOT, "skills", "**", "*.md"), recursive=True))
    total = failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for path in files:
            for line, src in blocks(path):
                total += 1
                err = check(src, tmp)
                if err:
                    failed += 1
                    print(f"{os.path.relpath(path, ROOT)}:{line}: {err}\n")
    print(f"{total} doc examples: {failed} failing")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
