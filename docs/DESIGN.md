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
    Point* q = alloc(Point);             // manual, no constructor call
    defer free(q);

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
- **Strings:** `string` is an immutable `{u8* ptr, usize len}` slice. Literals
  go in `.rodata`, so strings work even when freestanding. Only `.` and
  interpolation allocate, which means they need the runtime/GC (or a
  user-supplied allocator).
- **`echo`** compiles to a call to `__wl_write(u8*, usize)`. The hosted
  runtime implements it with the `write` syscall. A kernel provides its own
  (VGA, serial...).
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
   │  sema.c         name resolution, imports, type checking, layout (sizes/offsets)
   ▼
   │  codegen_x64.c  AST → x86-64 Intel-syntax asm (.s)
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
  types.c/.h      type representation, sizes, alignment
  sema.c/.h       resolution + type checking
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

**GC strategy:** mark-and-sweep, **conservative** stack scanning: any word
on the stack that looks like a pointer into the GC heap counts as a root.
That way the codegen doesn't have to emit precise stack maps. Objects from
`new` carry a small header. Memory from `alloc` is never scanned or
collected.

**The runtime is written in C** at first and linked in through our own FFI.
Later it can be rewritten in watrlang itself.

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
| 9  | defer + error unions   | `defer`, `T!E`, `?`                                       |
| 10 | Classes                | fields, methods, `__construct`, `->`                      |
| 11 | GC                     | `new`, `.` concat, interpolation, mark-and-sweep          |
| 12 | foreach, enums, arrays | `foreach (x as k => v)`, `enum`, `Enum::Value`, PHP-style `array` |
| 13 | Freestanding           | `--freestanding`, inline asm, `volatile`, `export`, packed/aligned structs |
| 14 | **Boot a kernel**      | multiboot kernel in QEMU prints to VGA, written in watrlang |
| 15 | Optimizer              | IR + register allocation                                  |
| ∞  | Self-hosting           | watrc rewritten in watrlang                               |

## 6. Settled after the first pass

- **Generics:** not in v1. Revisit after M12.
- **Inheritance / interfaces:** not in v1. Revisit after M12.
- **Integer division:** C-style. `/` on two ints truncates toward zero;
  `%` takes the sign of the left operand. Dividing by zero is a CPU fault
  (`#DE`), same as C on x86.
- **Null:** C-style. `null` is the zero pointer, assignable to any `T*`.
  Dereferencing it is not checked by the compiler.
- **Floats:** in v1, built in M5 alongside the integer types (`f32`, `f64`,
  `float`), using SSE registers per the System V ABI.
- **PHP-style arrays:** yes. `array` is a built-in GC-managed ordered map,
  like PHP's: keys are `int` or `string`, values can be any type, and
  insertion order is kept. Values are stored tagged, and you read a typed
  value back out with a runtime-checked `as`. Because it needs the GC,
  `array` is a compile error under `--no-gc` / `--freestanding`. Built in M12.

```php
array user = ["name" => "watr", "age" => 20];
user[] = "appended";                  // next int key, like PHP
int age = user["age"] as int;         // checked at runtime
foreach (user as k => v) { echo "{k}\n"; }
```
