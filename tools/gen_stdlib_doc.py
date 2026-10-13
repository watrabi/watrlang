#!/usr/bin/env python3
"""Writes docs/STDLIB.md from the comments and declarations in std/*.watr.

Every public declaration is listed with the `//` comment block right above
it. Private ones, and helpers whose names start with `_`, are left out.

    python3 tools/gen_stdlib_doc.py
"""
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STD = os.path.join(ROOT, "std")
ORDER = ["io", "str", "fmt", "math", "vec", "map", "sort", "arrays", "fs", "os", "time", "rand", "json", "hash", "crypto", "password", "mem", "gc", "sys"]

DECL = re.compile(r"^(?:(?:public|static|abstract)\s+)*(function|class|interface|enum|struct|const)\b")


def code_only(line):
    """The line without string/char literals and // comments, for counting braces."""
    line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    line = re.sub(r"'(\\.|[^'\\])*'", "''", line)
    return line.split("//")[0]


def signature(lines, i):
    """The declaration starting at line i, up to its body or `;`."""
    text = lines[i].strip()
    j = i
    while "{" not in text and not text.endswith(";") and j + 1 < len(lines):
        j += 1
        text += " " + lines[j].strip()
    if text.startswith("const "):
        return text.rstrip(";").strip()
    if text.startswith(("enum ", "struct ")):   # show the whole small body
        body = text
        body = code_only(body)
        while "}" not in body and j + 1 < len(lines):
            j += 1
            body += " " + code_only(lines[j]).strip()
        return re.sub(r"\s+", " ", body)
    return text.split("{")[0].rstrip(";").strip()


def comment_above(lines, i):
    out = []
    k = i - 1
    while k >= 0 and lines[k].strip().startswith("//"):
        out.insert(0, lines[k].strip()[2:].strip())
        k -= 1
    return " ".join(x for x in out if x)


def module_doc(lines):
    out = []
    for line in lines:
        s = line.strip()
        if not s.startswith("//"):
            break
        out.append(s[2:][1:] if s[2:].startswith(" ") else s[2:])
    return out


def main():
    mods = sorted(f[:-5] for f in os.listdir(STD) if f.endswith(".watr"))
    mods = [m for m in ORDER if m in mods] + [m for m in mods if m not in ORDER]
    out = ["# watrlang standard library", "",
           "Generated from the sources by `tools/gen_stdlib_doc.py`; don't edit by hand.",
           "Import a module with `import \"std/NAME\";` and call its functions as",
           "`NAME::function(...)` (or unqualified, when no other import uses the name).", ""]
    out.append("Modules: " + ", ".join(f"[`std/{m}`](#stdmodule-{m})" for m in mods) + ".")
    out.append("")
    for m in mods:
        lines = open(os.path.join(STD, m + ".watr")).read().split("\n")
        out.append(f"<a id=\"stdmodule-{m}\"></a>")
        out.append(f"## std/{m}")
        out.append("")
        doc = module_doc(lines)
        in_code = False
        for d in doc:
            if d.startswith("    ") and not in_code:
                out.append("```watr")
                in_code = True
            elif not d.startswith("    ") and in_code:
                out.append("```")
                in_code = False
            out.append(d[4:] if in_code else d)
        if in_code:
            out.append("```")
        out.append("")
        imports = [l.strip() for l in lines if l.startswith("import ")]
        depth = 0
        cls = None
        for i, line in enumerate(lines):
            s = line.strip()
            if depth == 0 or (cls and depth == 1):
                mt = DECL.match(s)
                if mt and not s.startswith("private") and "private" not in s.split("function")[0] and not re.search(r"(function|class|struct|enum)\s+_", s):
                    sig = signature(lines, i)
                    note = comment_above(lines, i)
                    if depth == 0 and mt.group(1) in ("class", "interface"):
                        out.append(f"### `{sig}`")
                        out.append("")
                        if note:
                            out.append(note)
                            out.append("")
                    elif depth == 0:
                        out.append(f"- `{sig}`" + (f" — {note}" if note else ""))
                    else:
                        out.append(f"  - `{sig}`" + (f" — {note}" if note else ""))
                if depth == 0 and mt and mt.group(1) in ("class", "interface"):
                    cls = sig
            code = code_only(line)
            depth += code.count("{") - code.count("}")
            if depth == 0 and cls and "}" in line:
                cls = None
                out.append("")
        out.append("")
    with open(os.path.join(ROOT, "docs", "STDLIB.md"), "w") as f:
        f.write("\n".join(out).rstrip() + "\n")
    print("wrote docs/STDLIB.md")


if __name__ == "__main__":
    main()
