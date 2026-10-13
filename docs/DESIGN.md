# watrlang design plan

watrlang is a compiled systems language with PHP-flavored syntax. It's built
to write real programs, all the way down to an operating system kernel.

- **Compiler:** written in plain C99, no dependencies (`watrc`)
- **Output:** x86-64 assembly (GNU `as`, Intel syntax), linked with `ld`
- **Targets:** Linux x86-64 (System V ABI), plus bare metal via `--freestanding`
- **Files:** `.watr`

## 1. Decisions so far

| Area            | Decision                                                                 |
|-----------------|--------------------------------------------------------------------------|
| Execution       | Ahead-of-time compiled to native code. No VM, no interpreter.            |
| Backend         | Our own codegen → `.s` text → GNU `as` + `ld`                            |
| Syntax          | PHP-flavored, cleaned up. **No `$` sigil.**                              |
| Typing          | Static, fully explicit, C-style declarations (`int x = 5;`)              |
| Int types       | `int` (=i64), `float` (=f64), `bool`, plus `i8..i64`, `u8..u64`, `f32`, `f64`, `usize` |
| Memory          | Manual (`alloc`/`free`) **and** optional GC (`new`). `new` is a compile error under `--no-gc` / `--freestanding`. |
| Errors          | Error-union return values + `?` propagation. No exceptions.              |
| Modules         | `import "std/io";`, one file = one module, no headers                    |
| Low-level       | Pointers + raw memory, `volatile`, inline asm, freestanding mode, C FFI  |
| PHP features    | `echo`, `"string {interp}"`, classes with `->`, `.` concat, `foreach (x as k => v)` |

## 2. What it looks like

### Hello world

```php
import "std/io";

function main(): int {
    string name = "world";
    echo "Hello, {name}!\n";
    return 0;
}
```

### Types, pointers, structs

```php
struct Vec2 {
    f32 x;
    f32 y;
}

function scale(Vec2* v, f32 k): void {
    v->x = v->x * k;       // -> works on values and pointers (auto-deref)
    v->y = v->y * k;
}

function main(): int {
    Vec2 v = Vec2 { x: 1.0, y: 2.0 };
    scale(&v, 3.0);

    u8 b = 255;
    int big = b as int;    // casts use `as`, not (int)b
    int* p = alloc(int, 16);   // manual: 16 ints
    defer free(p);             // runs when the scope exits
    p[0] = 42;
    return 0;
}
```

### Classes (PHP style)

```php
class Point {
    public int x;
    public int y;

    function __construct(int x, int y) {
        this->x = x;
        this->y = y;
    }

    function len2(): int {
        return this->x * this->x + this->y * this->y;
    }
}

function main(): int {
    Point p = new Point(3, 4);           // GC-managed
    echo "len2 = " . p->len2() . "\n";   // . is concat
    return 0;
}
```

### foreach

```php
int[5] nums = [1, 2, 3, 4, 5];
foreach (nums as n) { echo "{n}\n"; }
foreach (nums as i => n) { echo "{i}: {n}\n"; }
```

### Errors

```php
enum IoError { NotFound, Denied }

// `File!IoError` means: returns a File, or an IoError
function open(string path): File!IoError {
    if (!exists(path)) { return IoError::NotFound; }
    ...
}

function load(): string!IoError {
    File f = open("config.txt")?;   // on error, return it to our caller
    defer close(f);
    return read_all(f)?;
}
```

### C interop

```php
extern function printf(u8* fmt, ...): i32;   // System V ABI call into libc
extern function malloc(usize n): void*;
```

### Kernel (freestanding)

```php
// watrc --freestanding -T linker.ld kernel.watr
volatile u16* VGA = 0xB8000 as volatile u16*;

export function kmain(): void {
    string msg = "Hello from watrlang!";
    for (usize i = 0; i < len(msg); i++) {
        VGA[i] = (msg[i] as u16) | 0x0F00;
    }
    while (true) { asm { "cli"; "hlt"; } }
}

function outb(u16 port, u8 val): void {
    asm("out dx, al" : in("dx") port, in("al") val);
}
```

## 3. Language rules worth pinning down

- **Lexical rules:**
  - No `$` anywhere. `$name` is an error that tells PHP users to drop the `$`.
  - Integers: `42`, `1_000_000`, `0xFF`, `0b1010`, `0o777`. Leading zeros
    (`0123`) are an error, since C reads them as octal and that surprises people.
  - Floats need digits on both sides of the dot (`1.5`, `2.5e-3`), so
    `1 . 2` and `x.y` stay concatenation.
  - Strings can span lines. Escapes: `\n \t \r \0 \\ \" \' \xHH \{ \}`.
    `{` always starts an interpolation, so write `\{` for a literal brace.
    A lone `}` is fine. An interpolation must close on the same line.
  - Char literals: `'a'`, `'\n'`, `'\x41'`, each a single byte (`u8`).
  - Comments: `//` and `/* */` (not nested). Identifiers are ASCII; strings
    and comments can hold any UTF-8.
- **Operator precedence**, from tightest to loosest. This follows Rust rather
  than C, so bitwise operators bind tighter than comparisons and
  `x & 1 == 0` means `(x & 1) == 0`. `.` sits below `+`/`-`, like PHP 8.

  | Level | Operators | Notes |
  |-------|-----------|-------|
  | postfix | `f()` `a[i]` `a[]` `->` `x++` `x--` `expr?` | |
  | prefix | `-` `!` `~` `&` `*` `++` `--` | |
  | cast | `as` | `-x as u8` is `(-x) as u8` |
  | multiply | `*` `/` `%` | |
  | add | `+` `-` | |
  | shift | `<<` `>>` | |
  | bitwise | `&`, then `^`, then `\|` | |
  | concat | `.` | `"a" . n + 1` is `"a" . (n + 1)` |
  | compare | `==` `!=` `<` `>` `<=` `>=` | can't chain: `a < b < c` is an error |
  | logical | `&&`, then `\|\|` | |
  | ternary | `c ? a : b` | right-associative |
  | assign | `=` `+=` `-=` `*=` `/=` `%=` `.=` `&=` `\|=` `^=` `<<=` `>>=` | right-associative |

- **`?` has two jobs.** After an expression, `?` means error propagation
  when the next token can't start an expression (`f()?;`, `f()?->x`).
  Otherwise it starts a ternary. So `f()?[0]` reads as a ternary; write
  `(f()?)[0]`.
- **`*` in casts:** inside `x as T`, a `*` followed by something that can
  start an expression is multiplication: `n as int * 2` is `(n as int) * 2`,
  while `addr as u8*;` is a pointer cast.
- **Return types are optional** and default to `void`:
  `function __construct(int x) { ... }`.
- **Struct literals** are `Name { field: value }`. The parser recognizes
  them by `Name {` followed by `}` or `field:`.
- **Variables start at zero.** `int x;` is `0`, never leftover stack
  garbage.
- **No shadowing inside a function.** A local can't reuse the name of
  another local or parameter that's still in scope (it may reuse a
  global's name). Separate blocks can reuse names.
- **Assignment is a statement, not a value.** `if (x = 5)` and
  `a = b = c` are errors. `++`/`--` still produce values (`arr[i++]`).
- **Evaluation order is left to right**, for operands and for function
  arguments alike.
- **Global initializers must be constant.** They're evaluated at compile
  time; `const` globals can be used in later initializers.
- **Return checking:** an `int` function must not be able to reach its end.
  `while (1) { ... }` with no `break` counts as never ending.
- **Type rules (milestone 5):**
  - `int` is `i64`, `float` is `f64`, and `usize` is `u64`; each pair is
    interchangeable. There's no `char`; bytes are `u8`, and `'a'` is a `u8`.
  - **Automatic conversions only when nothing can be lost:** a smaller
    integer into a larger one that holds all its values (`u8` → `int`,
    `i16` → `i32`, `u32` → `i64`), `f32` → `f64`, `T*` → `void*`,
    `T*` → `const T*` / `volatile T*`, and `null` → any pointer. Everything
    else needs `as`.
  - **Signed and unsigned never mix implicitly.** `i32 + u32` is an error;
    `u8 + int` is fine (the `u8` widens).
  - **Literals adapt to their context:** `u8 x = 200;` types 200 as `u8`,
    and `x + 1` does `u8` arithmetic. A literal that doesn't fit is an
    error (`u8 x = 300;`).
  - **Integer arithmetic wraps at the type's width**: `u8` 250 + 10 is 4.
  - **`bool` is strict.** Conditions must be `bool` (`while (true)`,
    `if (n != 0)`, `if (p != null)`), `!`/`&&`/`||` take bools, and
    `b as int` converts to 0/1. Numbers don't convert to bool, not even
    with `as`; compare instead.
  - **`as` casts:** any numeric to any numeric (ints wrap, floats truncate
    toward zero), `bool` → int, pointer ↔ pointer, and pointer ↔ 64-bit
    int. Converting a float that's NaN or out of the target's range to an
    integer gives an unspecified value (but never crashes).
  - **Pointers:** `&x`, `*p`, `p[i]`, `p + n`, `p - q` (in elements),
    `p->field` (follows the pointer), comparisons. `void*` can't be
    dereferenced or used in arithmetic until cast.
  - **Structs** use C layout (fields aligned, size rounded to the
    alignment), copy by value on assignment, and can't be compared. In a
    literal, fields that aren't listed are zero: `Color { r: 255 }`.
  - **Fixed arrays** `T[N]` need a constant `N`, copy by value, and are
    bounds-checked: a constant index is checked at compile time, others at
    run time. An out-of-bounds index prints
    `panic at file:line:col: index out of bounds` and exits with status 101.
    Array literals must list every element.
  - `sizeof(T)` and `len(array)` are compile-time `usize` constants.
  - Structs and arrays can't be passed to or returned from functions by
    value yet (it needs the full System V classification, milestone 6);
    pass a pointer.
  - Returning the address of a local variable is an error (E0333).
- **C interop (milestone 6):**
  - `extern function name(...): T;` declares a C function; `...` makes it
    variadic (only extern functions can be). Variadic arguments get C's
    promotions (`f32` → `f64`), and calls set `al` as the ABI requires.
  - `extern T name;` declares a C global (accessed through the GOT).
  - `export function` / `export T name` keep their plain symbol names so C
    can call or read them. Everything else is `w.name` internally.
  - Structs pass and return by value following System V exactly: up to
    16 bytes in registers (each eightbyte INTEGER or SSE), larger ones in
    memory, with a hidden pointer for struct returns. Arrays don't pass by
    value. `tests/abi_fuzz.py` checks this against gcc.
  - A program that uses `extern` C functions, or is given extra
    `.c`/`.o`/`.a` files, is linked with the system C compiler and the C
    runtime; the C runtime calls the compiler-generated `main`. Otherwise
    the binary is static, libc-free, and starts at watrlang's own `_start`.
  - `alloc(T)` / `alloc(T, n)` return zeroed memory; running out of
    memory (or a size that overflows) panics. `free(p)` releases it.
    Since milestone 11 this memory comes from watrlang's own heap, not
    `malloc`, so `free` only takes pointers from `alloc`; to free memory
    that C allocated, declare `extern function free(void* p): void;`.
  - Until the `string` type exists, a string literal can be passed where
    a `u8*` is expected; it's stored NUL-terminated in read-only memory.
- **Strings (milestone 7):** `string` is an immutable `{const u8* ptr;
  usize len}` value (16 bytes, passed in two registers like the C struct).
  Literals live in read-only memory and are NUL-terminated, as are strings
  built at run time, so `s->ptr` can always go to C. Operations:
  `len(s)`, `s[i]` (a bounds-checked `u8`), `s->ptr`, `s->len`, `.` and
  `.=` (numbers, bools and pointers convert to text automatically),
  `"{interpolation}"`, and `==`/`<`/... comparing bytes.
  `string { ptr: p, len: n }` wraps existing bytes without copying.
  Strings built at run time live on the garbage-collected heap.
- **`echo`** prints strings, numbers (floats like C's `%.14g`), bools
  (`true`/`false`) and pointers (hex, or `null`). In a static binary it
  writes straight to fd 1; when libc is linked it goes through stdio, so it
  interleaves correctly with `printf`. A kernel will supply its own output.
- **The runtime is written in watrlang** (`runtime/prelude.watr`), compiled
  into every program; only what a program uses ends up in its binary.
  The compiler provides three primitives in assembly: write to stdout,
  mmap, and float formatting (which uses libc's snprintf, so programs that
  print floats link libc).
- **Dead code:** only functions reachable from `main` or an `export` are
  compiled. Unused functions get warning W0004.
- **Modules (milestone 8):** each file is a module. `import "path";`
  loads `path.watr` relative to the importing file; `import "std/x";`
  loads from the standard library (`std/` next to watrc, or `$WATR_STD`).
  Every file is loaded once, and import cycles are fine.
  - Imported functions, structs and globals can be used unqualified, or
    as `alias::name` (the alias is the last part of the path, or set with
    `import "x" as y;`). A file's own definitions win over imported ones;
    a name defined by two imports must be qualified (E0341).
  - Imports aren't transitive: you see what you import, not what your
    imports import.
  - `private` keeps a declaration inside its module.
  - Each module's functions get their own symbol prefix (`w.std.str.trim`),
    so modules can reuse names freely; exported names must stay unique.
  - Standard library so far: `std/io` (print, println), `std/math` (int
    helpers, PI/E, and libm functions), `std/str` (substr, index_of,
    contains, starts_with, ends_with, to_upper, to_lower, trim, repeat,
    parse_int, from_cstr), `std/mem` (copy, set, equal), `std/gc`
    (collect, live_bytes, heap_bytes, collections), `std/arrays` (see below).
- **Enums (milestone 9):** `enum Color { Red, Green = 5, Blue }` declares
  named integer constants with their own type. The underlying type is
  `i32` (like C) unless given: `enum Http: u16 { ... }`. Variants without
  a value count up from the previous one, starting at 0. Values are
  written `Color::Red` (or `mod::Color::Red` from another module).
  - An enum compares (`==`, `<`, ...) only with the same enum, and does no
    arithmetic; `as` converts to and from integers.
  - `echo`, interpolation and `.` print the variant's name (`Red`); a
    value that isn't a variant (from `as`) prints as its number. Each enum
    that's printed gets a small generated function for this.
- **Error unions (milestone 9):** `T!E` holds either a `T` or an error
  from enum `E`; `void!E` is "fine, or an error". It's laid out like
  `struct { bool is_err; E err; T value; }` and passed like that struct.
  - A `T` or an `E` converts to `T!E` implicitly, so `return 5;` and
    `return ParseError::Empty;` both work in an `int!ParseError` function.
    A `void!E` function that reaches its end succeeds.
  - `r?` gives r's value, or returns r's error from the current function
    (which must return an error union with the same `E`), running defers.
  - `r ?? fallback` gives r's value, or `fallback` if r is an error.
  - `r->ok` is true for a value; `r->value` and `r->error` read the parts
    and panic if the other one is there. None of them can be assigned.
  - Throwing away an error union result (`save();`) is warning W0005.
- **`defer` (milestone 9):** `defer stmt;` runs `stmt` when the enclosing
  block exits: at its closing brace, or through `return`, `break`,
  `continue` or `?`. Several defers run newest first. A defer only runs if
  control reached it. `return`'s value is computed before defers run.
  Deferred code can't itself `return`, `break`/`continue` out of the defer,
  or use `?` (E0346).
- **Classes (milestone 10):** a class value is a *reference* to an object
  on the heap, like in PHP: it's 8 bytes, can be `null`, and copying it
  shares the object. `new C(args)` allocates a zeroed object, sets field
  defaults (`public int n = 10;`, which must be constants), then runs
  `__construct(args)` if there is one.
  - Methods use `this`; `static function` methods have no `this` and are
    called as `C::name()` (or `mod::C::name()`).
  - Members are public unless marked `private`, which limits them to the
    class's own methods.
  - `__toString(): string` makes objects printable with `echo`,
    interpolation and `.`.
  - `==`/`!=` compare identity. Using a field or calling a method through
    `null` panics with the location, instead of crashing.
  - Objects live on the garbage-collected heap (milestone 11). Code that
    manages memory by hand uses structs and `alloc`/`free`. Inheritance
    and interfaces are still deferred.
- **Arrays and `mixed` (milestone 12):**
  - `mixed` (PHP's name for "any type") holds null, a bool, an int, a
    float, a string, an array or an object, with a tag saying which. It's
    16 bytes, `{tag, bits}`; strings are boxed. Any of those types converts
    to `mixed` implicitly; getting a value out takes `as`, which panics
    with the location if the type is wrong (`m as int`, `m as Point`).
    An int reads as a float too. `mixed` values print, interpolate and
    join with `.`, and `==`/`!=` compare them strictly (same type and
    value, like PHP's `===`).
  - `array` is a reference to the map, like objects: assigning or passing
    one shares it (`std/arrays::copy` makes a separate one). `array a;`
    starts as an empty array, and so do array fields of classes; a null
    array (in a global or struct) reads as empty, and writing to one
    panics.
  - `[1, 2]` and `["k" => v]` make arrays (a literal is a fixed array only
    where a `T[N]` is expected). `a[k]` reads an element (null if there's
    none), `a[k] = v` sets one (keeping its place if it exists), `a[] = v`
    appends with one more than the largest int key so far, and `len(a)`
    counts. Elements are `mixed`, so `a[k][j]` works on nested arrays.
    Compound assignment (`a[k] += 1`) isn't allowed on elements.
  - The map keeps entries in insertion order with a hash table of entry
    numbers (linear probing); removing leaves a hole that's skipped, and
    holes are squeezed out when the entries are full.
  - `std/arrays`: has, remove, push, pop, keys, values, contains, key_of,
    join, range, copy, merge, reverse, type_of. `tests/array_fuzz.py`
    checks the map against a model of PHP's semantics.
- **`foreach` (milestone 12):** `foreach (items as v)` or
  `foreach (items as k => v)` loops over an `array` (k and v are `mixed`),
  a fixed array (k is the `usize` index) or a string (v is each `u8`).
  Bindings may declare a type, `foreach (scores as string name => int n)`,
  which converts each one (checked at run time for arrays). The bindings
  are copies; the loop sees elements appended during it. The compiler
  rewrites `foreach` into a `for` loop, so `break` and `continue` work as
  usual.
- **`->` vs `.`:** `->` is *always* member access, auto-dereferencing
  pointers. `.` is *only* concatenation. They never conflict.
- **Pointer declarations without a sigil:** `Foo* x;` could also parse as
  multiplication. Rule: *if a statement can parse as a declaration, it is
  one* (C++ uses the same rule). `a * b;` as a statement is pointless anyway.
- **Casts:** `x as T`, never `(T)x`. This avoids the C cast/paren ambiguity.
- **No implicit conversions** between int types except widening
  (`u8` → `int`). Narrowing needs `as`.
- **Arrays:** `int[5]` is fixed and lives on the stack. `int[]` is a slice
  (ptr+len). `len(x)` works on both.
- **Classes v1:** fields + methods + `__construct`, `public`/`private`.
  No inheritance yet (needs vtables, planned for later).
- **Integer overflow:** wraps (two's complement), defined behavior. No UB games.

## 4. Compiler architecture (C)

```
source.watr
   │  lexer.c        tokens (with file:line:col for errors)
   ▼
   │  parser.c       recursive descent + Pratt for expressions → AST
   ▼
   │  sema.c         name resolution, type checking, struct layout, constant
   │                 folding; annotates the AST with types and symbols and
   │                 reports every "is this program valid" error
   ▼
   │  codegen_x64.c  annotated AST → x86-64 Intel-syntax asm (.s); reports nothing
   ▼
 as + ld             → ELF executable / kernel image
```

Planned layout:

```
src/
  main.c          CLI: watrc [--freestanding] [--no-gc] [-o out] [-S] file.watr
  arena.c/.h      arena allocator: the compiler never calls free()
  diag.c/.h       error messages with source line + caret
  lexer.c/.h
  ast.h           node types (tagged unions)
  parser.c/.h
  types.c/.h      type representation, interning, conversion rules
  sema.c/.h       resolution + type checking
  explain.c       long-form text for `watrc --explain CODE`
  codegen_x64.c   assembly emitter
runtime/
  start.s         _start → main → exit syscall
  rt.c            __wl_write, string concat/format, alloc/free
  gc.c            mark-and-sweep (conservative stack scan)
std/
  io.watr, mem.watr, str.watr ...
tests/
  *.watr + expected output, run_tests.sh
docs/
  DESIGN.md
```

**Codegen strategy:** start with a simple **stack-machine style** emitter.
Every expression leaves its result in `rax`, and temporaries are pushed and
popped. The output is slow but correct, and it's the fastest way to get
working binaries. Once the language works, add a linear IR and a simple
register allocator (linear scan) as an optimization phase.

**GC (milestone 11):** mark-and-sweep, written in watrlang in the runtime
prelude, with **conservative** root scanning: any word that looks like a
pointer into the heap counts. That way the codegen doesn't have to emit
precise stack maps.

- The heap is chunks of memory from mmap (1 MiB, or one per object over
  256 KiB). Every allocation has a 16-byte header (size and flags). A
  bitmap per chunk records where objects start, so pointers into the
  middle of an object keep it alive; string slices rely on this.
- Roots: the stack from `main`'s caller down to the collector, the
  callee-saved registers, globals whose types can hold pointers (codegen
  emits a table of them, `w.__gc_roots`), and every `alloc` block.
- Objects are scanned word by word too, except string bytes (allocated
  "no-scan"). Dead neighbors merge into free blocks, kept on exact-size
  free lists up to 1 KiB and a first-fit list above that.
- A collection runs when the bytes allocated since the last one exceed
  max(4 MiB, the bytes that survived it).
- `alloc` blocks share the heap but are never collected; `free` returns
  them. Pointers to GC objects kept only in memory from C's `malloc`
  don't keep them alive.
- `std/gc` has `collect()`, `live_bytes()`, `heap_bytes()` and
  `collections()`. Compiling with `WATR_GC_STRESS=1` in the environment
  produces a program that collects at every allocation; the test suite
  runs that way too.
- Being conservative, a stale pointer left in a stack slot can keep
  garbage alive until the slot is overwritten.

**Symbols:** watrlang functions are emitted as `w.<name>` (e.g. `w.main`).
That stops a function called `rax` from assembling as `call rax`, and keeps
watrlang names apart from C names once we link against libc. `export` and
`extern` functions keep their plain names so C and linker scripts can find
them. A hosted program starts at our own `_start`, which calls `w.main`
and passes its return value to the `exit` syscall.

**Diagnostics** (`src/diag.c`) are modeled on Rust's: every error has a
code (`E0xxx` lexing, `E01xx` syntax, `E02xx` names, `E03xx` types and
flow, `E09xx` not yet implemented, `W0xxx` warnings), a primary span with
a label, optional secondary spans, `= note:`/`= help:` lines, and
suggested fixes rendered as the corrected source line. `watrc --explain
CODE` prints the long form from `src/explain.c`; the test suite fails if
a code is used without an explanation. Unused-variable and unreachable
warnings are suppressed in functions that already have errors, where
they'd mostly be noise.

**The runtime** is a watrlang prelude (see the strings rules above) plus a
few assembly primitives emitted by codegen.

## 5. Milestones

Each milestone ends with tests passing and something you can run.

| #  | Milestone              | Done when...                                              |
|----|------------------------|-----------------------------------------------------------|
| 0  | Skeleton               | Makefile, arena, diagnostics, `watrc --version`           |
| 1  | Lexer                  | `watrc --tokens file.watr` dumps every token correctly      |
| 2  | Parser                 | `watrc --ast file.watr` pretty-prints the AST               |
| 3  | First binary           | `function main(): int { return 40 + 2; }`: exit code 42   |
| 4  | Control flow + locals  | if/else, while, for, recursion (`fib(30)` works)          |
| 5  | Type system            | all int sizes, floats (SSE), bool, pointers, `as`, structs, fixed arrays |
| 6  | C FFI                  | `extern function printf(...)` prints via libc             |
| 7  | Strings + echo         | literals, `echo`, `len`, slices                           |
| 8  | Modules                | `import`, multi-file programs, a start on `std/`          |
| 9  | defer, enums, errors   | `defer`, `enum`, `Enum::Value`, `T!E`, `?`, `??`          |
| 10 | Classes                | fields, methods, `__construct`, `static`, `__toString`    |
| 11 | GC                     | mark-and-sweep for `new` objects and strings; `alloc` in the same heap |
| 12 | foreach, arrays        | `foreach (x as k => v)`, PHP-style `array`, `mixed`       |
| 13 | Freestanding           | `--freestanding`, inline asm, `volatile`, `export`, packed/aligned structs |
| 14 | **Boot a kernel**      | multiboot kernel in QEMU prints to VGA, written in watrlang |
| 15 | Optimizer              | IR + register allocation                                  |
| ∞  | Self-hosting           | watrc rewritten in watrlang                               |

## 6. Settled after the first pass

- **Generics:** not in v1. Revisit after M12.
- **Inheritance / interfaces:** not in v1. Revisit after M12.
- **Integer division:** C-style. `/` on two ints truncates toward zero;
  `%` takes the sign of the left operand. Dividing by zero is a CPU fault
  (`#DE`), same as C on x86. So is the one overflowing division,
  `-9223372036854775808 / -1`.
- **Shifts:** the shift count is taken mod 64 (what x86 does), so
  `1 << 65 == 2`. `>>` on signed ints is arithmetic, keeping the sign.
- **Null:** C-style. `null` is the zero pointer, assignable to any `T*`.
  Dereferencing it is not checked by the compiler.
- **Floats:** in v1, built in M5 alongside the integer types (`f32`, `f64`,
  `float`), using SSE registers per the System V ABI.
- **PHP-style arrays:** yes. `array` is a built-in GC-managed ordered map,
  like PHP's: keys are `int` or `string`, values can be any type, and
  insertion order is kept. Values are stored tagged, and you read a typed
  value back out with a runtime-checked `as`. Because it needs the GC,
  `array` will be a compile error under `--freestanding`. Built in M12;
  see "Arrays and `mixed`" above for the details.

```php
array user = ["name" => "watr", "age" => 20];
user[] = "appended";                  // next int key, like PHP
int age = user["age"] as int;         // checked at runtime
foreach (user as k => v) { echo "{k}\n"; }
```
