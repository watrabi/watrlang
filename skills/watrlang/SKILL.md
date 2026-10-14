---
name: watrlang
description: Write, fix, explain, port, or review code in watrlang (.watr files, compiled with `watrc`), a statically typed systems language with PHP-flavored syntax (echo, "string {interpolation}", `.` concatenation, arrow-style member access, foreach, classes) on a C-like core (pointers, structs, alloc/free, inline asm, generics, error unions, bare-metal kernels). Use this skill whenever the user mentions watrlang, watr, watrc, a .watr file, or asks for a program "in watr", even for a tiny snippet, because watrlang looks like PHP and C but differs from both in ways that make plausible-looking code fail to compile.
---

# Writing watrlang

watrlang compiles to native x86-64 code for Linux (and for Windows with
`watrc --target windows`, which writes a .exe). It reads like PHP without
the `$`, with C's explicit types. Most mistakes come from carrying over a
PHP, C or JavaScript habit that watrlang deliberately rejects, so the rules
below are worth reading closely even when the syntax looks familiar.

## Workflow

1. **Write the program** following the rules and cheat sheet below. When a
   feature isn't covered here, look it up in `references/LANGUAGE.md` (the
   full language guide) or `references/STDLIB.md` (every standard library
   signature) rather than guessing: the language is small and specific.
   `references/examples.md` has complete idiomatic programs to model on.
2. **Compile it if `watrc` is available** (`watrc prog.watr` writes
   `./prog`, `-o name` picks another name; in the watrlang repo it is
   `build/watrc`, built with `make`). Then run it. Compiling is the only reliable check, and it's cheap.
3. **Fix errors from the messages.** watrc's diagnostics are Rust-style and
   precise: the code, the exact span, and usually a `help:` line with the
   corrected source. Apply the suggested fix rather than rewriting around
   it. `watrc --explain E0310` explains any error code at length. Fix the
   first error first; later ones are often consequences.
4. Warnings (unused variables W0001, unused functions W0004, ignored error
   results W0005) don't stop compilation, but clean code has none; prefix
   intentionally unused names with `_`.

## The rules that trip people up

1. **Types come first, no `$`**: `int count = 0;`, `string name = "x";`.
   Every variable, parameter, field and return type is declared.
2. **`->` for every member access**, on objects, struct values and
   pointers alike: `p->x`, `this->items`, `r->ok`. **`.` is only string
   concatenation** (`"a" . b`). `p.x` is an error.
3. **`+` never joins strings.** Use `.` or interpolation: `"{a} and {b}"`.
   Interpolation takes any expression: `"{x + 1} {obj->name()}"`.
4. **Conditions must be `bool`.** `if (n != 0)`, `if (ptr != null)`,
   `while (true)`. No truthiness, and `as bool` doesn't exist.
5. **Casts are `x as T`**, never `(T)x`. Implicit conversions only widen
   losslessly (`u8` → `int`, `f32` → `f64`); everything else needs `as`.
6. **Signed and unsigned never mix.** `len()` returns `usize`, so write
   `for (usize i = 0; i < len(a); i++)` or compare with `len(a) as int`.
   `int + 1.5` is an error too: `n as f64 + 1.5`.
7. **`main` is `function main(): int`** with no parameters; its result is
   the exit status. Command-line arguments: `import "std/os";` then
   `os::args()` (a `Vec<string>`, element 0 is the program).
8. **No shadowing inside a function.** A local can't reuse the name of a
   live local or parameter (`int n` twice is an error). Pick new names.
9. **Assignment is a statement**: no `if (x = f())`, no `a = b = 0`.
10. **Function syntax**: `function name(int a, string b): RetType { ... }`.
    No return type means `void`. No default arguments, no overloading
    (one function per name), no nested functions, closures or function
    pointers. Methods also use the `function` keyword.
11. **No `switch`, no exceptions.** Use `if`/`else if`. Failure is an error
    union: `function parse(string s): int!ParseError`, returning either an
    `int` or `ParseError::Bad` (an enum value); callers use `f()?` to pass
    errors up or `f() ?? fallback` to supply a default.
12. **Classes are references** (GC-managed, nullable): `Foo f = new
    Foo(1);`. Structs are values: `Point p = Point { x: 1, y: 2 };`
    (unlisted fields are zero). Structs have no methods.
13. **Field defaults must be constants**, there are no static fields (use a
    global), and a field and a method can't share a name.
14. **Strings are immutable bytes.** `s[i]` is a `u8` (it prints as a
    number); `len(s)` is bytes. Build strings with `.`/`.=`/interpolation.
    Escapes are `\n \t \r \0 \\ \" \' \xHH \{ \}`; a literal `{` must be
    written `\{` (a `$` needs no escape). String positions in `std/str`
    are `usize` (`substr(s, start, n)`) while `index_of` returns `int` (-1
    when absent), so convert with `as usize` after checking it.
15. **Arrays**: `int[5] a = [1, 2, 3, 4, 5];` is fixed-size and a literal
    must list every element. Array types read left to right: `int[3][4]` is
    4 rows of `int[3]`. For growable lists use `Vec<T>` (`std/vec`); for
    maps use `Map<K, V>` (`std/map`). PHP's dynamic `array` exists but its
    elements are `mixed` and need `as` to read typed.
16. **Keywords can't be names**, including type names: no variable or
    method called `int`, `string`, `float`, `array`, `echo`, `new`...
17. **Return checking is exact.** A non-void function must not be able to
    fall off its end, but a `while (true)` loop without `break` counts as
    never ending: write no `return` after it (one there is warning W0002,
    unreachable code). A `void!E` function may simply end; that's success.

## Cheat sheet

```watr
import "std/vec";                    // modules: std/NAME or a relative path
import "std/str";

const int LIMIT = 3;                 // globals: constant initializers only
int calls = 0;

enum Color { Red, Green, Blue }      // Color::Red; prints as "Red"
enum Fail { Empty, TooBig }

struct Point {                       // a value type, C layout
    int x;
    int y;
}

class Counter {                      // a reference type
    private int n = 0;
    public string label;

    public function __construct(string label) { this->label = label; }

    public function bump(int by): int {
        this->n += by;
        return this->n;
    }

    public static function make(): Counter { return new Counter("default"); }

    public function __toString(): string { return "{this->label}={this->n}"; }
}

function first_word(string s): string!Fail {      // a string, or a Fail
    if (len(s) == 0) {
        return Fail::Empty;
    }
    vec::Vec<string> words = str::words(s);
    return words->get(0);
}

function biggest<T>(vec::Vec<T> v): T {           // generic, inferred at calls
    T best = v->get(0);
    foreach (v as x) {
        if (x > best) {
            best = x;
        }
    }
    return best;
}

function main(): int {
    Point p = Point { x: 1, y: 2 };
    p->x = 10;
    Point* pp = &p;
    pp->y += 1;

    Counter c = Counter::make();
    c->bump(2);
    calls++;

    vec::Vec<int> nums = new vec::Vec<int>();
    for (int i = 0; i < LIMIT; i++) {
        nums->push(i * i);
    }

    string w = first_word("hello world") ?? "?";
    string!Fail r = first_word("");
    if (!r->ok) {
        echo "error: ", r->error, "\n";
    }

    int[4] fixed = [4, 3, 2, 1];
    int sum = 0;
    for (usize i = 0; i < len(fixed); i++) {
        sum += fixed[i];
    }

    f64 avg = sum as f64 / 4.0;
    echo "{p->x},{p->y} {c} {w} {biggest(nums)} {avg}\n";
    echo Color::Green, " ", calls, " ", sum > 5 ? "big" : "small", "\n";
    return 0;
}
```

### Quick reference

| Want | Write |
|------|-------|
| print | `echo a, " ", b, "\n";` |
| integer types | `int` (=i64), `i8..i64`, `u8..u64`, `usize` (=u64); `float` (=f64), `f32`; `bool`; no `char` (bytes are `u8`, `'a'` is a `u8`) |
| loops | `for (int i = 0; i < n; i++)`, `while (c)`, `do { } while (c);`, `foreach (xs as x)`, `foreach (xs as i => x)`, `break`, `continue` |
| foreach works on | fixed arrays, strings (bytes), `array`, and objects with `len()`/`get(i)` such as `Vec<T>` |
| ternary / logic | `c ? a : b`, `&&`, `\|\|`, `!` (bools only) |
| bit ops | `& \| ^ ~ << >>`; they bind tighter than `==`, so `x & 1 == 0` is `(x & 1) == 0` |
| integer division | truncates; `%` has the dividend's sign; arithmetic wraps |
| pointers | `int* p = &x; *p = 1; p[2]; p + 1; p->field; null` |
| manual memory | `int* buf = alloc(int, n); defer free(buf);` (zeroed) |
| cleanup | `defer stmt;` runs at block exit, newest first |
| errors | `T!E` where E is an enum and T is any type (int, string, a class, a struct...); `return E::X;`; `v = f()?;`; `f() ?? d`; `r->ok`, `r->value`, `r->error`; `??` doesn't work on `void!E` (test `->ok`) |
| inheritance | `class B extends A implements I, J`, `abstract`, `protected`, `parent::m()`, `parent::__construct(...)`, `x instanceof C`, `x as C` (checked downcast); a subclass without its own `__construct` inherits the parent's |
| interfaces | `interface I { function f(int a): int; }` |
| generics | `function f<T>(T a): T`, `class Box<T>`, `struct Pair<A, B>`, `new Box<int>()`, `Pair<int, string> { first: 1, second: "a" }`, `f<u8>(x)`. The body is checked per use, so `<`, `==`, `+` and interpolation work on T whenever the actual type supports them (numbers, strings, enums for `<`/`==`). Generic classes can refer to themselves (`Node<T> left;`) and recurse |
| static method | `public static function make(): C` called as `C::make()` |
| size/length | `sizeof(T)`, `len(array_or_string)` (both `usize`) |
| modules | `import "std/str";` → `str::trim(s)` or unqualified `trim(s)`; `import "x" as y;`; `private` hides a top-level declaration |
| C | `extern function puts(const u8* s): i32;`, `export function f()`, pass `s->ptr` for a C string |
| hashing | `hash_of(x)` builtin (numbers, strings, enums, pointers, objects) |

### Standard library at a glance

Full signatures: `references/STDLIB.md`. Most used:

- `std/str`: `substr(s, start, n)`, `index_of`, `contains`, `starts_with`,
  `ends_with`, `trim`, `to_upper`, `to_lower`, `replace(s, from, to)`,
  `split(s, sep)` → `Vec<string>`, `join(parts, sep)`, `words`, `lines`,
  `to_int(s)` → `int!ParseError`, `to_float`, `repeat`, `reverse`.
- `std/vec`: `new vec::Vec<T>()`; `push`, `pop`, `get(i)`, `set(i, v)`,
  `len()`, `insert`, `remove(i)`, `contains`, `index_of`, `slice(a, b)`,
  `join(sep)`, `reverse`, `clear`. Indexes and `len()` are `int`, so in
  `foreach (v as i => x)` the index `i` is an `int`. `v->join(", ")` works
  for any printable element type; `str::join(parts, sep)` takes a
  `Vec<string>`.
- `std/map`: `new map::Map<K, V>()`; `set(k, v)`, `get(k)` → `V!MapError`,
  `get_or(k, d)`, `has`, `remove`, `keys()`, `values()`, `len()`.
- `std/sort`: `sort::vec(v)`, `sort::vec_by(v, cmp)` where `cmp` implements
  `sort::Less<T>` (`function less(T a, T b): bool`), `sort::search`.
- `std/fmt`: `fixed(x, 2)`, `pad_left(s, w, ' ')`, `pad_right`, `hex(v, 0)`,
  `grouped(n)`, `format("% of %", [a, b])` (`%` placeholders, because `{}`
  is interpolation).
- `std/fs`: `read(path)` → `string!fs::IoError`, `write`, `append` →
  `bool!fs::IoError`, `exists`, `remove`, `describe(err)`.
- `std/os`: `args()`, `getenv(name)`, `exit(code)`.
- `std/math`: `abs`, `min`, `max`, `clamp`, `sqrt`, `pow`, `floor`, `PI`.
- `std/time`, `std/rand` (`new rand::Rng(seed)`, `r->range(lo, hi)`),
  `std/io` (`println`), `std/arrays` (PHP-array helpers).

### Naming

Fields and methods share one namespace per class, so a getter can't be
named like its field. Prefer a plain noun for the method and a different
field name (`private int count_;` with `public function count(): int`),
or `get_`/`set_` methods. Unused results of non-error calls aren't
warned about; unused locals and parameters are (prefix with `_`).

## Pitfalls checklist

Before finishing, scan the code for these; each one is a compile error:

- a `.` used for a field or method (should be `->`), or `+` on strings;
- `if (x)` / `while (n)` / `!n` on a non-bool;
- an `int` compared with `len(...)` or another `usize`;
- mixing `int` and `f64` in arithmetic without `as`;
- a missing type on a variable, parameter or field, or a `$`;
- a redeclared local name (including reusing a loop variable name while
  the first is still in scope, or naming a local like a parameter);
- `else if` is fine, but there's no `elseif` and no `switch`;
- an array literal with fewer elements than its declared size;
- a struct value expected to have methods, or a class used without `new`;
- `??` applied to a `void!E` result, or an error union used as its value
  without `?`/`??`;
- a keyword used as a name (`int`, `float`, `string`, `new`, `echo`...);
- PHP functions that don't exist (`strlen`, `count`, `explode`,
  `array_push`, `printf` without declaring it `extern`): use `len`,
  `str::split`, `v->push`, `echo`.

When targeting bare metal (`--freestanding`), heap features (`new`,
`alloc`, `array`, `mixed`, building strings at run time) are unavailable;
see section 24 of `references/LANGUAGE.md`.
