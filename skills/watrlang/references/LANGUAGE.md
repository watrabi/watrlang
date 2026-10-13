# The watrlang language

watrlang is a compiled systems language with PHP-flavored syntax: `echo`,
`"string {interpolation}"`, `.` for concatenation, `->` for members,
`foreach (x as k => v)`, classes with `__construct`. Underneath it is a C-like
language with explicit static types, pointers, structs, manual memory when you
want it, a garbage collector when you don't, inline assembly, and a
`--freestanding` mode for writing kernels. It compiles straight to x86-64
machine code for Linux.

This is the complete guide and reference. Every example in it is compiled by
`tools/check_docs.py` (part of `make test`), so the examples are kept true.

- [1. Getting started](#1-getting-started)
- [2. A tour](#2-a-tour)
- [3. Lexical structure](#3-lexical-structure)
- [4. Types](#4-types)
- [5. Variables and constants](#5-variables-and-constants)
- [6. Operators](#6-operators)
- [7. Control flow](#7-control-flow)
- [8. Functions](#8-functions)
- [9. Strings and echo](#9-strings-and-echo)
- [10. Pointers and manual memory](#10-pointers-and-manual-memory)
- [11. Structs](#11-structs)
- [12. Fixed arrays](#12-fixed-arrays)
- [13. Enums](#13-enums)
- [14. Errors: `T!E`, `?` and `??`](#14-errors-te--and-)
- [15. `defer`](#15-defer)
- [16. Classes](#16-classes)
- [17. Inheritance and interfaces](#17-inheritance-and-interfaces)
- [18. Generics](#18-generics)
- [19. PHP-style `array` and `mixed`](#19-php-style-array-and-mixed)
- [20. Modules](#20-modules)
- [21. The standard library](#21-the-standard-library)
- [22. Calling C, and being called from C](#22-calling-c-and-being-called-from-c)
- [23. Inline assembly](#23-inline-assembly)
- [24. Bare metal: `--freestanding`](#24-bare-metal---freestanding)
- [25. Runtime checks and panics](#25-runtime-checks-and-panics)
- [26. The compiler](#26-the-compiler)
- [27. What watrlang doesn't have](#27-what-watrlang-doesnt-have)
- [28. Coming from PHP, C, or JavaScript](#28-coming-from-php-c-or-javascript)

## 1. Getting started

Build the compiler (a C99 compiler, GNU `as` and `ld` are all it needs):

```sh
make            # builds build/watrc
make test       # the test suite, fuzzers, and a kernel booted in QEMU
```

Write `hello.watr`:

```watr
function main(): int {
    string name = "world";
    echo "Hello, {name}!\n";
    return 0;
}
```

Compile and run it:

```sh
build/watrc hello.watr      # writes ./hello
./hello                     # Hello, world!
```

`main` takes no parameters and returns `int`, which becomes the exit status.
Command-line arguments come from `std/os` (`os::args()`).

## 2. A tour

```watr
import "std/vec";
import "std/str";

enum Suit { Hearts, Spades }

struct Card {
    int rank;
    Suit suit;
}

class Deck {
    private vec::Vec<Card> cards;

    public function __construct() {
        this->cards = new vec::Vec<Card>();
        for (int r = 1; r <= 3; r++) {
            this->cards->push(Card { rank: r, suit: Suit::Hearts });
            this->cards->push(Card { rank: r, suit: Suit::Spades });
        }
    }

    public function total(): int {
        int sum = 0;
        foreach (this->cards as c) {
            sum += c->rank;
        }
        return sum;
    }

    public function count(): int { return this->cards->len(); }
}

function describe(Card c): string {
    return "{c->rank} of {c->suit}";
}

function main(): int {
    Deck d = new Deck();
    echo "cards: ", d->count(), ", total rank: ", d->total(), "\n";
    echo describe(Card { rank: 7, suit: Suit::Spades }), "\n";
    echo str::to_upper("done") . "!\n";
    return 0;
}
```

Things to notice: types come first (`int sum = 0;`), there is no `$`, members
are always reached with `->` (on objects, struct values and pointers alike),
and `.` only joins strings.

## 3. Lexical structure

- **Comments:** `// to end of line` and `/* block */` (not nested).
- **Identifiers:** ASCII letters, digits and `_`, not starting with a digit.
  No `$` sigil; `$name` is an error that tells you to drop the `$`.
- **Keywords:** `function return if else while do for foreach as break
  continue struct class enum interface extends implements abstract public
  private protected static instanceof import extern export const volatile
  defer new alloc sizeof echo asm this true false null`, and the type names
  `void bool int float string array mixed i8 i16 i32 i64 u8 u16 u32 u64 f32
  f64 usize`. Keywords can't be used as names, not even for methods
  (`r->int()` is an error).
- **Integer literals:** `42`, `1_000_000`, `0xFF`, `0b1010`, `0o777`. A
  leading zero (`0123`) is an error, since C would read it as octal.
- **Float literals** need digits on both sides of the point: `1.5`, `0.25`,
  `2.5e-3`. (`1.` and `.5` are not floats; this keeps `x.y` concatenation.)
- **Char literals** are single bytes of type `u8`: `'a'`, `'\n'`, `'\x41'`.
- **String literals** use double quotes and may span lines. Escapes: `\n \t
  \r \0 \\ \" \' \xHH \{ \}`. A `{` always starts an interpolation, so write
  `\{` for a literal brace.
- **Statements** end with `;`. Blocks use `{ }`, and `if`/`while`/`for`
  bodies may be single statements, though braces are the norm.

## 4. Types

| Type | What it is |
|------|------------|
| `i8` `i16` `i32` `i64` | signed integers; `int` is `i64` |
| `u8` `u16` `u32` `u64` | unsigned integers; `usize` is `u64` |
| `f32` `f64` | IEEE floats; `float` is `f64` |
| `bool` | `true` / `false` |
| `T*` | pointer to T (`const T*`, `volatile T*`, `void*`) |
| `T[N]` | fixed array of N elements, stored inline |
| `string` | immutable text: `{const u8* ptr; usize len}` |
| `struct S` | a C-layout value type |
| `enum E` | named integer constants |
| `T!E` | a T, or an error from enum E |
| `class C` | reference to a garbage-collected object (may be `null`) |
| `array` | PHP-style ordered map (a reference) |
| `mixed` | any value, tagged with its type |
| `void` | no value (function results, `void*`) |

`int`/`i64`, `float`/`f64` and `usize`/`u64` are the same types under two
names. There is no `char`: bytes are `u8`.

### Conversions

watrlang converts implicitly only when nothing can be lost:

- a smaller integer into a larger one that holds all its values (`u8` →
  `int`, `i16` → `i32`, `u32` → `i64`), and `f32` → `f64`;
- `T*` → `void*`, `T*` → `const T*`, `null` → any pointer or class;
- a subclass → its base class or an interface it implements;
- a `T` or an `E` → `T!E`; almost anything → `mixed`.

Everything else uses `as`:

```watr
function main(): int {
    f64 x = 3.9;
    int n = x as int;          // 3: truncates toward zero
    u8 b = 300 as u8;          // 44: integers wrap
    int t = true as int;       // 1
    u64 addr = (&n) as u64;    // pointer to integer
    i32 small = n as i32;
    echo n, " ", b, " ", t, " ", small, " ", addr != 0, "\n";
    return 0;
}
```

Signed and unsigned integers never mix implicitly: `i32 + u32` is an error,
and so is comparing an `int` with a `usize`. The usual case is `len()`, which
returns `usize`:

```watr
function main(): int {
    int[4] a = [1, 2, 3, 4];
    int sum = 0;
    for (usize i = 0; i < len(a); i++) {     // usize counter, or...
        sum += a[i];
    }
    for (int i = 0; i < len(a) as int; i++) { // ...convert the length
        sum += a[i];
    }
    echo sum, "\n";
    return 0;
}
```

**Literals adapt to their context.** `u8 x = 200;` types `200` as a `u8`, and
`x + 1` is `u8` arithmetic. A literal that doesn't fit is an error.

**`bool` is strict.** Conditions must be `bool`: write `if (n != 0)`,
`if (p != null)`, `while (true)`. Numbers don't convert to `bool`, not even
with `as`.

## 5. Variables and constants

```watr
int counter = 0;                  // a global: its initializer must be constant
const int LIMIT = 10;             // a constant global
u8[4] magic = [0x7F, 0x45, 0x4C, 0x46];

function main(): int {
    int x;                        // locals start at zero
    int y = 5;
    const f64 rate = 0.5;         // can't be assigned again
    x = y * 2;
    counter++;
    echo x, " ", rate, " ", LIMIT, " ", counter, " ", magic[1], "\n";
    return 0;
}
```

- Every variable starts at zero (or null, or empty) unless initialized.
- **No shadowing inside a function**: a local can't reuse the name of another
  local or parameter that's still in scope. Separate blocks may reuse names.
- **Assignment is a statement, not an expression**: `if (x = 5)` and
  `a = b = c` are errors. `x++` and `x--` do produce values (`arr[i++]`).
- Global initializers are evaluated at compile time; `const` globals can be
  used in later ones.

## 6. Operators

From tightest to loosest. This follows Rust rather than C: bitwise operators
bind tighter than comparisons, so `x & 1 == 0` means `(x & 1) == 0`.

| Level | Operators |
|-------|-----------|
| postfix | `f()` `a[i]` `a[]` `->` `x++` `x--` `expr?` |
| prefix | `-` `!` `~` `&` (address) `*` (deref) `++` `--` |
| cast | `x as T`, `x instanceof C` |
| multiply | `*` `/` `%` |
| add | `+` `-` |
| shift | `<<` `>>` |
| bitwise | `&`, then `^`, then `\|` |
| concat | `.` |
| compare | `==` `!=` `<` `>` `<=` `>=` (can't be chained: `a < b < c` is an error) |
| logical | `&&`, then `\|\|` (short-circuit) |
| coalesce | `??` |
| ternary | `c ? a : b` |
| assign | `=` `+=` `-=` `*=` `/=` `%=` `.=` `&=` `\|=` `^=` `<<=` `>>=` |

Semantics worth knowing:

- Integer arithmetic **wraps** at the type's width (`u8` 250 + 10 is 4).
- `/` on integers truncates toward zero and `%` takes the dividend's sign,
  like C. Dividing by zero is a CPU fault.
- Shift counts are taken mod 64; `>>` on a signed type keeps the sign.
- `.` converts numbers, bools, enums, pointers and objects with
  `__toString` to text: `"n = " . n`. It sits below `+`, so
  `"a" . n + 1` is `"a" . (n + 1)`.
- Strings compare by content with `==` and `<`; objects compare by identity;
  structs can't be compared.
- Evaluation is left to right, for operands and arguments alike.

## 7. Control flow

```watr
function sign(int n): string {
    if (n < 0) {
        return "negative";
    } else if (n == 0) {
        return "zero";
    } else {
        return "positive";
    }
}

function main(): int {
    int i = 0;
    while (i < 3) {
        i++;
    }
    do {
        i--;
    } while (i > 0);
    for (int k = 0; k < 10; k++) {
        if (k == 2) { continue; }
        if (k == 5) { break; }
        echo k, " ";
    }
    int[3] xs = [10, 20, 30];
    foreach (xs as x) { echo x, " "; }
    foreach (xs as idx => x) { echo idx, ":", x, " "; }
    foreach ("hi" as byte) { echo byte, " "; }   // bytes (u8) of a string
    echo sign(-4), " ", 3 > 2 ? "yes" : "no", "\n";
    return 0;
}
```

- `foreach` works on fixed arrays (the key is the `usize` index), strings
  (each byte, a `u8`), PHP-style `array`s (keys and values are `mixed`), and
  any object whose class has `len()` and `get(int i)` methods, like
  `Vec<T>`. Bindings can declare a type to convert to:
  `foreach (scores as string name => int n)`.
- There is no `switch`; use `if`/`else if`.
- An `int` function must not be able to reach its end without `return`.
  `while (true)` without a `break` counts as never ending.

## 8. Functions

```watr
function add(int a, int b): int {
    return a + b;
}

function greet(string who) {          // no return type means void
    echo "hi ", who, "\n";
}

function fib(int n): int {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

function main(): int {
    greet("ada");
    echo add(2, 3), " ", fib(10), " ", later(), "\n";
    return 0;
}

function later(): int { return 7; }   // order doesn't matter
```

- Parameters are passed by value; pass a pointer (`int* out`) to let a
  function change the caller's variable. Structs pass and return by value
  (following the C ABI); fixed arrays can't, so pass a pointer to them.
- No default arguments, no overloading (one function per name per module),
  no nested functions or closures, and no function pointers yet.
- To return several things, return a struct.
- Functions only reachable from nowhere are dropped, with warning W0004
  ("never used"). Prefix a name with `_` to silence it.

## 9. Strings and echo

`string` is an immutable byte string. Literals live in read-only memory;
strings built at run time live on the garbage-collected heap. Every string
is NUL-terminated, so `s->ptr` can be passed to C.

```watr
import "std/str";

function main(): int {
    string name = "Ada";
    int age = 36;
    string s = "name: " . name . ", age: " . age;   // . joins
    s .= "!";
    string t = "{name} is {age + 1} next year";     // any expression in { }
    echo s, "\n", t, "\n";
    echo len(name), " ", name[0], " ", name == "Ada", " ", "abc" < "abd", "\n";
    echo str::substr(name, 1, 2), " ", str::index_of(name, "d"), "\n";
    echo "a literal brace: \{\n";
    return 0;
}
```

- `len(s)` is the length in bytes; `s[i]` is a bounds-checked `u8`.
- `s->ptr` and `s->len` read the parts; `string { ptr: p, len: n }` wraps
  existing bytes without copying.
- `echo a, b, c;` prints each value: strings as text, numbers (floats like
  C's `%.14g`), bools as `true`/`false`, pointers in hex (or `null`), enums
  by variant name, objects through `__toString()`.
- A `u8` prints as a number. To get a one-character string, take a
  substring (`str::substr(s, i, 1)`) or wrap a byte:
  `string { ptr: &c, len: 1 }`.
- `std/str` has the rest: split, join, replace, trim, to_upper, to_int,
  to_float... and `std/fmt` formats numbers (padding, hex, fixed decimals).

## 10. Pointers and manual memory

```watr
struct Node {
    int value;
    Node* next;
}

function sum(Node* n): int {
    int total = 0;
    while (n != null) {
        total += n->value;
        n = n->next;
    }
    return total;
}

function main(): int {
    int x = 5;
    int* p = &x;
    *p = 6;
    int* nums = alloc(int, 4);       // zeroed, manual memory
    defer free(nums);                // freed when main returns
    nums[2] = 9;
    int* third = nums + 2;           // pointer arithmetic is in elements
    Node b = Node { value: 2, next: null };
    Node a = Node { value: 1, next: &b };
    echo x, " ", *third, " ", sum(&a), "\n";
    return 0;
}
```

- `&x`, `*p`, `p[i]`, `p + n`, `p - q` (in elements), `p->field`, and
  comparisons. `void*` must be cast before use.
- `alloc(T)` / `alloc(T, n)` return zeroed memory; `free(p)` releases it.
  Running out of memory panics. This memory is never moved or collected.
- Objects from `new` and strings built at run time are garbage collected
  instead (a conservative mark-and-sweep collector; `std/gc` controls it).
- Returning the address of a local is an error.
- `const T*` can't be written through; `volatile T*` makes every access a
  real load or store (for memory-mapped hardware).

## 11. Structs

```watr
struct Vec2 {
    f64 x;
    f64 y;
}

function add(Vec2 a, Vec2 b): Vec2 {
    return Vec2 { x: a->x + b->x, y: a->y + b->y };
}

function scale(Vec2* v, f64 k) {
    v->x = v->x * k;            // -> works through pointers too
    v->y = v->y * k;
}

#[packed]
struct Header {
    u8 kind;
    u32 size;                   // no padding before it
}

function main(): int {
    Vec2 a = Vec2 { x: 1.0, y: 2.0 };
    Vec2 b = Vec2 { x: 0.5 };   // unlisted fields are zero
    Vec2 c = add(a, b);
    scale(&c, 2.0);
    Vec2 copy = c;              // structs copy by value
    copy->x = 0.0;
    echo c->x, " ", c->y, " ", copy->x, " ", sizeof(Header), "\n";
    return 0;
}
```

- Fields use C layout and alignment; `#[packed]` removes padding and
  `#[align(N)]` raises a struct's (or global's) alignment.
- Structs are values: assignment copies, they can't be compared with `==`,
  and they can't have methods (use a class, or functions taking `S*`).
- Fields are read with `->`, never `.`.

## 12. Fixed arrays

```watr
int[5] squares;

function fill(int[5]* arr) {
    for (int i = 0; i < 5; i++) {
        arr[0][i] = i * i;
    }
}

function main(): int {
    int[3] a = [1, 2, 3];           // a literal must list every element
    int[3][4] grid;                 // 4 rows, each an int[3]
    grid[3][2] = 7;                 // row 3, column 2
    fill(&squares);
    int* first = &a[0];             // decays by taking the address
    echo len(a), " ", len(grid), " ", grid[3][2], " ", squares[4], " ", first[1], "\n";
    return 0;
}
```

- `T[N]` needs a constant N, is stored inline, and copies by value.
- Array types read left to right: `int[3][4]` is an array of 4 `int[3]`s
  (the reverse of C's `int g[4][3]`), so `grid[row][col]` has row < 4 and
  col < 3.
- Indexes are **bounds-checked**: a constant index at compile time, others
  at run time (an out-of-range index panics). The optimizer removes checks
  it can prove, like those inside `for (i = 0; i < N; i++)`.
- `len(arr)` and `sizeof(T)` are compile-time `usize` constants.
- To pass an array, pass a pointer: `int[5]* p` (index it as `p[0][i]`), or
  a pointer to the first element with a length.
- For growable lists use `Vec<T>` from `std/vec`.

## 13. Enums

```watr
enum Color { Red, Green = 5, Blue }       // Blue is 6
enum Http: u16 { Ok = 200, NotFound = 404 }

function main(): int {
    Color c = Color::Green;
    if (c == Color::Green) {
        echo c, " ", c as int, " ", Color::Blue as int, "\n";   // Green 5 6
    }
    Http h = 404 as Http;
    echo h, " ", Http::Ok as int, "\n";                          // NotFound 200
    return 0;
}
```

- The underlying type is `i32` unless given (`enum Http: u16`).
- Enums compare only with the same enum and do no arithmetic; `as` converts
  to and from integers. They print as their variant name.

## 14. Errors: `T!E`, `?` and `??`

watrlang has no exceptions. A function that can fail returns an **error
union** `T!E`: either a `T`, or an error from the enum `E`.

```watr
enum ParseError { Empty, NotDigit }

function digit(string s): int!ParseError {
    if (len(s) == 0) {
        return ParseError::Empty;          // an E converts to T!E
    }
    if (s[0] < '0' || s[0] > '9') {
        return ParseError::NotDigit;
    }
    return (s[0] - '0') as int;            // so does a T
}

function sum(string a, string b): int!ParseError {
    int x = digit(a)?;                     // ? returns the error to our caller
    int y = digit(b)?;
    return x + y;
}

function check(int n): void!ParseError {   // void!E: fine, or an error
    if (n < 0) {
        return ParseError::NotDigit;
    }
}

function main(): int {
    echo sum("3", "4") ?? -1, "\n";        // 7    (?? gives a fallback)
    echo sum("3", "x") ?? -1, "\n";        // -1
    int!ParseError r = sum("", "1");
    if (!r->ok) {
        echo "failed: ", r->error, "\n";   // failed: Empty
    } else {
        echo r->value, "\n";
    }
    if (!check(-1)->ok) {
        echo "check failed\n";
    }
    return 0;
}
```

- `r?` gives the value, or returns the error from the current function
  (which must return an error union with the same `E`), running defers.
- `r ?? fallback` gives the value or the fallback. It needs a value, so it
  doesn't apply to `void!E`; test `->ok` instead.
- `r->ok`, `r->value`, `r->error` read the parts (reading the wrong one
  panics).
- Ignoring an error union result (`save();`) is warning W0005.
- Unrecoverable problems (bugs) are panics, not errors: see
  [section 25](#25-runtime-checks-and-panics).

## 15. `defer`

`defer stmt;` runs `stmt` when the enclosing block exits: at its closing
brace, or through `return`, `break`, `continue` or `?`. Several defers run
newest first.

```watr
function work(): int {
    echo "open\n";
    defer echo "close\n";              // runs last
    defer echo "flush\n";              // runs first
    for (int i = 0; i < 2; i++) {
        defer echo "end of iteration\n";
        echo "iteration ", i, "\n";
    }
    return 42;                         // the value is computed before defers run
}

function main(): int {
    echo work(), "\n";
    return 0;
}
```

A defer only runs if control reached it. Deferred code can't `return`, use
`?`, or `break`/`continue` out of itself.

## 16. Classes

A class value is a **reference** to a garbage-collected object, as in PHP:
copying it shares the object, and it can be `null`.

```watr
class Account {
    public string owner;
    private int cents = 0;            // field defaults must be constants

    public function __construct(string owner) {
        this->owner = owner;
    }

    public function deposit(int amount): void {
        this->cents += amount;
    }

    public function balance(): int {
        return this->cents;
    }

    public static function create(string owner): Account {
        return new Account(owner);
    }

    public function __toString(): string {
        return "{this->owner}: {this->cents}";
    }
}

function main(): int {
    Account a = new Account("ada");
    a->deposit(50);
    Account same = a;                 // the same object
    same->deposit(25);
    Account b = Account::create("alan");
    echo a, " | ", b, " | ", a->balance(), " ", a == same, " ", a == b, "\n";
    Account none = null;
    if (none == null) {
        echo "no account\n";
    }
    return 0;
}
```

- `new C(args)` allocates a zeroed object, applies field defaults, then
  calls `__construct(args)` if the class has one.
- Members are public unless marked `private` (class only) or `protected`
  (class and subclasses).
- Methods reach the object as `this`; `static` methods have no `this` and
  are called as `C::name()`. There are no static fields: use a global next
  to the class.
- Fields and methods share one namespace, so a field and a method can't
  have the same name (`balance` and `balance()`).
- `__toString(): string` makes objects printable with `echo`, `.` and
  interpolation.
- `==` compares identity. Using a member through `null` panics with the
  source location instead of crashing.
- Methods must be declared with `function`; they can be called before
  their definition in the file.

## 17. Inheritance and interfaces

```watr
interface Shape {
    function area(): f64;
    function name(): string;
}

abstract class Base implements Shape {
    protected string label;

    public function __construct(string label) { this->label = label; }

    public function name(): string { return this->label; }

    abstract public function area(): f64;

    public function describe(): string {
        return "{this->name()} with area {this->area()}";
    }
}

class Rect extends Base {
    private f64 w;
    private f64 h;

    public function __construct(f64 w, f64 h) {
        parent::__construct("rect");
        this->w = w;
        this->h = h;
    }

    public function area(): f64 { return this->w * this->h; }
}

class Square extends Rect {
    public function __construct(f64 side) { parent::__construct(side, side); }

    public function name(): string { return "square (" . parent::name() . ")"; }
}

function main(): int {
    Shape s = new Square(3.0);            // a subclass converts to its bases
    echo s->name(), " ", s->area(), "\n";
    Base b = new Rect(2.0, 5.0);
    echo b->describe(), "\n";
    if (s instanceof Rect) {
        Rect r = s as Rect;               // downcast, checked at run time
        echo r->area(), "\n";
    }
    return 0;
}
```

- Single inheritance with `extends`, any number of interfaces with
  `implements`; interfaces can `extends` other interfaces.
- Methods are virtual (except private and static ones). An override takes
  the same parameter types and returns the same type or a subclass.
- `parent::method(...)` calls the base version; constructors are inherited,
  and `parent::__construct(...)` calls the base one.
- `abstract class` can't be instantiated; `abstract function f(): T;` has no
  body and must be implemented by concrete subclasses. Interface methods
  are written `function f(): T;`.
- `x instanceof C` tests (false for null); `x as C` downcasts and panics if
  x isn't a C (null passes through).

## 18. Generics

Functions, structs, classes and interfaces can take type parameters. Each use
with different types is compiled separately (like C++ templates and Rust), so
generic code is as fast as hand-written code.

```watr
function max<T>(T a, T b): T {
    return a > b ? a : b;
}

struct Pair<A, B> {
    A first;
    B second;
}

function swap<A, B>(Pair<A, B> p): Pair<B, A> {
    return Pair<B, A> { first: p->second, second: p->first };
}

interface Container<T> {
    function size(): int;
}

class Stack<T> implements Container<T> {
    private T[16] items;
    private int n;

    public function push(T v): void {
        this->items[this->n] = v;
        this->n++;
    }

    public function pop(): T {
        this->n--;
        return this->items[this->n];
    }

    public function size(): int { return this->n; }
}

function main(): int {
    echo max(3, 7), " ", max(2.5, 1.5), " ", max("pear", "apple"), "\n";
    echo max<u8>(200, 100), "\n";             // explicit type arguments
    Pair<int, string> p = Pair<int, string> { first: 1, second: "one" };
    Pair<string, int> q = swap(p);            // types inferred from p
    echo q->first, " ", q->second, "\n";
    Stack<string> s = new Stack<string>();
    s->push("a");
    s->push("b");
    Container<string> c = s;
    echo s->pop(), " ", c->size(), "\n";
    return 0;
}
```

- Uses name the types: `Box<int>`, `new Stack<string>()`,
  `Pair<int, u8*> { ... }`, nested `Box<Box<int>>`.
- Calls usually infer them from the arguments. Literals adapt: `max(x, 1)`
  with an `i32` x is `max<i32>`, and `max(1, 2.5)` is `max<f64>`. When
  nothing decides a type parameter (`make()`), write it: `make<int>()`.
- There are no constraints to declare: the body is checked for each set of
  types it's used with, and an error points at the generic code with a note
  saying which instance and which use caused it. Methods of a generic class
  are only checked when something calls them.
- A class can extend an instance: `class IntList extends List<int>`.
- Methods can't have their own type parameters; put them on the class or
  write a generic function.

## 19. PHP-style `array` and `mixed`

For dynamic data, watrlang has PHP's ordered map, `array`, and a tagged
any-value type, `mixed`. Typed code (structs, `Vec<T>`, `Map<K, V>`) is faster
and checked at compile time; `array` is for when the shape is dynamic.

```watr
import "std/arrays";

function main(): int {
    array a = [1, "two", 3.5];          // keys 0, 1, 2
    a["name"] = "ada";                  // string keys keep insertion order
    a[] = true;                         // append: key 3
    echo len(a), " ", a[1], " ", a["name"], "\n";
    foreach (a as k => v) {
        echo k, "=", v, " ";
    }
    echo "\n";
    mixed m = a[0];
    int n = m as int + 1;               // `as` checks the type at run time
    echo n, " ", arrays::type_of(a[2]), " ", arrays::has(a, "name"), "\n";
    array nested = ["xs" => [1, 2, 3]];
    echo nested["xs"][2], "\n";
    return 0;
}
```

- `array` is a reference (assigning shares it; `arrays::copy` copies). A
  fresh `array a;` is empty. Keys are ints or strings; values are `mixed`.
- `a[k]` reads (null if missing), `a[k] = v` writes, `a[] = v` appends,
  `len(a)` counts. Compound assignment on elements (`a[k] += 1`) isn't
  allowed; write `a[k] = a[k] as int + 1`.
- `mixed` holds null, a bool, an int, a float, a string, an array or an
  object. Any of those converts to `mixed` implicitly; `m as T` gets it out
  and panics with the location if the type is wrong. `==` on mixed values is
  strict (same type and value).

## 20. Modules

Each file is a module. `import "path";` loads `path.watr` relative to the
importing file; `import "std/x";` loads from the standard library.

```watr
import "std/str";
import "std/math" as m;

function main(): int {
    echo str::trim("  hi  "), " ", m::abs(-5), " ", to_upper("ok"), "\n";
    return 0;
}
```

- Imported names can be used qualified (`str::trim`) or unqualified
  (`to_upper`) when no other import defines the same name. The qualifier is
  the last part of the path, or the name after `as`.
- Types too: `vec::Vec<int>`, `fs::IoError::NotFound`.
- `private` at the top level keeps a declaration inside its module.
- Imports aren't transitive: you see only what you import yourself. Every
  file is loaded once, and import cycles are fine.

## 21. The standard library

Written in watrlang, and needing no C library. Full signatures are in
[STDLIB.md](STDLIB.md); in short:

| Module | Contents |
|--------|----------|
| `std/io` | `print`, `println` |
| `std/str` | substr, index_of, contains, starts/ends_with, to_upper/lower, trim, repeat, split, join, words, lines, replace, count, reverse, to_int, to_float, is_digit... |
| `std/fmt` | pad_left/right, center, hex, bin, oct, fixed, grouped, bytes, `format("% and %", [a, b])` |
| `std/math` | abs, min, max, clamp, gcd, ipow, PI, E, and sqrt, pow, sin, cos, floor... (from libm) |
| `std/vec` | `Vec<T>`: push, pop, get, set, insert, remove, slice, join, ... |
| `std/map` | `Map<K, V>`: set, get, get_or, has, remove, keys, values (insertion ordered) |
| `std/sort` | stable sorting of Vecs and pointer ranges, by `<` or a `Less<T>`; search, min, max |
| `std/arrays` | helpers for PHP-style arrays |
| `std/fs` | read, write, append, exists, size, remove, rename, mkdir, rmdir (`T!IoError`) |
| `std/os` | args, argc, getenv, has_env, exit, pid |
| `std/time` | now_ns, elapsed_ms, unix, sleep_ms, utc, format |
| `std/rand` | `Rng` (seeded, repeatable) and module-level range, unit, chance, shuffle, pick |
| `std/json` | encode, pretty, decode, valid: PHP-compatible JSON for `array`/`mixed` |
| `std/hash` | SHA-256, SHA-512, SHA-1, MD5, BLAKE2b, HMAC, HKDF, PBKDF2 |
| `std/crypto` | secure random bytes/ints/tokens, constant-time equal, hex, base64(url), XChaCha20-Poly1305 seal/open, ChaCha20, Poly1305, signed expiring tokens |
| `std/password` | PHP-compatible password hash/verify/needs_rehash: Argon2id (and Argon2i/d), bcrypt, PBKDF2 formats; derive_key |
| `std/mem` | copy, set, equal on raw bytes |
| `std/gc` | collect, live_bytes, heap_bytes |
| `std/sys` | raw Linux system calls |

```watr
import "std/vec";
import "std/map";
import "std/sort";
import "std/str";
import "std/fmt";

function main(): int {
    vec::Vec<string> words = str::split("the quick brown fox the end", " ");
    map::Map<string, int> counts = new map::Map<string, int>();
    foreach (words as w) {
        counts->set(w, counts->get_or(w, 0) + 1);
    }
    vec::Vec<string> keys = counts->keys();
    sort::vec(keys);
    foreach (keys as k) {
        echo fmt::pad_right(k, 6, '.'), " ", counts->get(k) ?? 0, "\n";
    }
    echo fmt::format("% words, % distinct", [words->len(), counts->len()]), "\n";
    return 0;
}
```

`hash_of(x)` is a builtin that hashes numbers, bools, enums, strings,
pointers, objects (by identity) and `mixed`; `Map` uses it for keys.

## 22. Calling C, and being called from C

```watr
extern function printf(const u8* fmt, ...): i32;
extern function abs(i32 x): i32;

export function watr_square(int x): int {      // callable from C as watr_square
    return x * x;
}

function main(): int {
    printf("%d %s\n", abs(-3), "from C");
    string s = "strings pass as their ptr";
    printf("%s\n", s->ptr);
    return 0;
}
```

- `extern function` declares a C function (`...` for variadic ones);
  `extern T name;` a C global. Calls follow the System V ABI exactly,
  including structs by value.
- `export` keeps a function's or global's plain symbol name so C can use it.
- Extra `.c`, `.o` and `.a` files on the command line are compiled and
  linked in: `watrc main.watr helpers.c`. Programs that use C are linked
  with the system C compiler; others are static and libc-free.
- `free` only takes memory from watrlang's `alloc`; to free memory C
  allocated, declare C's: `extern function free(void* p): void;` in a
  module that doesn't use watrlang's `free`.

## 23. Inline assembly

```watr
function add(int a, int b): int {
    int r;
    asm("mov rax, rdi"
        "add rax, rsi"
        : in("rdi") a, in("rsi") b, out("rax") r);
    return r;
}

function swap_bytes(u32 x): u32 {
    u32 r = x;
    asm("bswap eax" : inout("eax") r);
    return r;
}

function main(): int {
    echo add(2, 3), " ", swap_bytes(0x11223344), "\n";
    return 0;
}
```

- Intel syntax, one string per line. `asm { "cli"; "hlt"; }` emits lines
  with no operands.
- Operands: `in("reg") expr`, `out("reg") lvalue`, `inout("reg") lvalue`,
  with any width of rax..r15 (not rsp/rbp) or xmm0..xmm15.
- Callee-saved registers the asm mentions are saved around it; others may
  be clobbered freely.

## 24. Bare metal: `--freestanding`

`watrc --freestanding` compiles for code with no operating system, such as a
kernel: no C library, no runtime heap, no `main`. Exported functions are the
entry points, called from your boot code; `-T script.ld` and `-e symbol` go
to the linker, and `-c` writes an object file. `examples/kernel` is a
complete kernel that boots in QEMU.

```watr
// watrc-flags: --freestanding -c
volatile u16* VGA = 0xB8000 as volatile u16*;

export function watr_write(const u8* p, usize n): void {
    for (usize i = 0; i < n; i++) {
        VGA[i] = p[i] as u16 | 0x0F00;
    }
}

function outb(u16 port, u8 value): void {
    asm("out dx, al" : in("dx") port, in("al") value);
}

export function kmain(): void {
    echo "Hello from watrlang";
    outb(0x3F8, 'A');
    while (true) {
        asm { "cli"; "hlt"; }
    }
}
```

- Everything that needs the heap is an error where it's used: `new`,
  `alloc`/`free`, `array`, `mixed`, building strings at run time. Printing
  floats needs libc. The rest of the language works.
- `echo` goes through `export function watr_write(const u8* p, usize n)`,
  which you provide. Failed runtime checks call `watr_panic(msg, len)` if
  you export one, and otherwise halt.
- `#[packed]`, `#[align(4096)]` and `volatile` are there for page tables,
  descriptor tables and memory-mapped devices.

## 25. Runtime checks and panics

Some bugs are caught at run time and stop the program with a message and exit
status 101:

```watr
// expect: exit 101
function main(): int {
    int[3] a = [1, 2, 3];
    int i = 5;
    echo a[i];          // panic at file:line:col: index out of bounds (array length 3)
    return 0;
}
```

Checked: array and string indexes, `Vec` indexes, member access and method
calls through `null`, downcasts with `as`, reading `mixed` as the wrong type,
reading the wrong part of an error union, and running out of memory. Not
checked: raw pointer accesses (a null or wild pointer crashes like in C),
integer overflow (it wraps), and division by zero (a CPU fault).

## 26. The compiler

```sh
watrc [options] file.watr [file.c file.o lib.a ...]
```

| Option | Effect |
|--------|--------|
| `-o path` | output file (default: the source name without `.watr`) |
| `-S` | write assembly instead of an executable |
| `-c` | write an object file |
| `-O0`, `-O1` | `-O1` (the default) optimizes: register allocation, inlining, constant folding, bounds-check removal; `-O0` is a simple stack machine |
| `--freestanding` | bare metal (section 24) |
| `-T script`, `-e symbol` | linker script and entry point, with `--freestanding` |
| `-v` | show the assembler and linker commands |
| `--explain CODE` | a longer explanation of an error, e.g. `--explain E0310` |
| `--tokens`, `--ast` | print the token stream or syntax tree |

Error messages follow Rust's style: the code, the source line, labels, notes,
and suggested fixes. Every error code has an explanation behind `--explain`.

```text
error[E0384]: `.` joins strings; members are reached with `->`
 --> p.watr:7:10
  |
7 |     echo p.x;
  |          ^^^ this reads as `p` joined with a variable `x`
  |
help: use `->`, which works on values and pointers alike
  |
7 |     echo p->x;
  |          ~~~~
```

Environment variables: `WATR_STD` (where the standard library is),
`WATR_GC_STRESS=1` (collect at every allocation, for testing),
`WATR_OPT=0` (default to -O0), `WATR_DUMP_IR=1` and `WATR_NO_INLINE=1`
(optimizer debugging).

## 27. What watrlang doesn't have

These aren't supported (yet), so don't reach for them:

- function pointers, closures, lambdas, nested functions;
- `switch`/`match` (use `if`/`else if`);
- default arguments, named arguments, overloading;
- exceptions (`try`/`catch`/`throw`; use `T!E`);
- methods on structs (use classes or functions taking `S*`);
- slices (`T[]`), and `main` parameters (use `os::args()`);
- generic methods (put the type parameter on the class);
- operator overloading, and `[]` indexing on objects (use `get(i)`);
- implicit truthiness (`if (n)`), `+` on strings, `.` for members;
- targets other than x86-64 Linux and bare metal.

## 28. Coming from PHP, C, or JavaScript

| You might write | In watrlang |
|-----------------|-------------|
| `$x = 5;` | `int x = 5;` (types first, no `$`) |
| `$obj->field` / `obj.field` / `s.x` | `obj->field`, `s->x` (always `->`) |
| `"a" + "b"` | `"a" . "b"` or `"{a}{b}"` |
| `if ($n)` / `if (ptr)` | `if (n != 0)`, `if (ptr != null)` |
| `(int)x` | `x as int` |
| `function f($a = 1)` | no defaults; write two functions |
| `throw new E()` / `try` | return `T!E`; use `?` and `??` |
| `array_push($a, $v)` | `a[] = v;`, or `Vec<T>` with `push` |
| `count($a)` / `strlen($s)` | `len(a)`, `len(s)` |
| `char c = 'a';` | `u8 c = 'a';` |
| `malloc` / `free` | `alloc(T, n)` / `free(p)`, or `new` for GC objects |
| `for (int i = 0; i < len(a); i++)` | `usize i`, or `len(a) as int` |
| `null` checks on objects | the same: `if (o == null)` |
| `switch` | `if` / `else if` chains |
