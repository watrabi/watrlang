# watrlang standard library

Generated from the sources by `tools/gen_stdlib_doc.py`; don't edit by hand.
Import a module with `import "std/NAME";` and call its functions as
`NAME::function(...)` (or unqualified, when no other import uses the name).

Modules: [`std/io`](#stdmodule-io), [`std/str`](#stdmodule-str), [`std/fmt`](#stdmodule-fmt), [`std/math`](#stdmodule-math), [`std/vec`](#stdmodule-vec), [`std/map`](#stdmodule-map), [`std/sort`](#stdmodule-sort), [`std/arrays`](#stdmodule-arrays), [`std/fs`](#stdmodule-fs), [`std/os`](#stdmodule-os), [`std/time`](#stdmodule-time), [`std/rand`](#stdmodule-rand), [`std/mem`](#stdmodule-mem), [`std/gc`](#stdmodule-gc), [`std/sys`](#stdmodule-sys).

<a id="stdmodule-io"></a>
## std/io

Console output.

- `function print(string s): void` — Prints a string.
- `function println(string s): void` — Prints a string and a newline.

<a id="stdmodule-str"></a>
## std/str

String utilities. Strings are immutable: these return new strings.

- `function substr(string s, usize start, usize n): string` — Bytes from s[start] for n bytes (clamped to the string).
- `function index_of(string s, string needle): int` — Where `needle` first appears in `s`, or -1.
- `function contains(string s, string needle): bool`
- `function starts_with(string s, string prefix): bool`
- `function ends_with(string s, string suffix): bool`
- `function to_upper(string s): string`
- `function to_lower(string s): string`
- `function is_space(u8 c): bool`
- `function trim(string s): string` — Without leading and trailing whitespace.
- `function repeat(string s, usize times): string`
- `function parse_int(string s): int` — Parses an optional '-' followed by decimal digits. Stops at the first other character.
- `function from_cstr(const u8* p): string` — Wraps a NUL-terminated C string (copying it).
- `function is_digit(u8 c): bool`
- `function is_alpha(u8 c): bool`
- `function is_alnum(u8 c): bool`
- `function trim_left(string s): string` — Without leading whitespace.
- `function trim_right(string s): string` — Without trailing whitespace.
- `function last_index_of(string s, string needle): int` — Where `needle` last appears in `s`, or -1.
- `function count(string s, string needle): int` — How many times `needle` appears, not overlapping.
- `function replace(string s, string from, string to): string` — s with every `from` replaced by `to`.
- `function split(string s, string sep): vec::Vec<string>` — The pieces of s between occurrences of `sep`: split("a,b,,c", ",") is ["a", "b", "", "c"]. An empty `sep` splits into single bytes.
- `function words(string s): vec::Vec<string>` — The words of s: runs of non-whitespace.
- `function lines(string s): vec::Vec<string>` — The lines of s, without their "\n" (or "\r\n"); no empty last line for a trailing newline.
- `function join(vec::Vec<string> parts, string sep): string` — The parts joined with `sep` between them.
- `function reverse(string s): string` — The bytes of s in reverse order.
- `enum ParseError { Empty, Invalid }`
- `function to_int(string s): int!ParseError` — A whole string as an integer: optional sign, then decimal digits, or "0x" and hex digits. Unlike parse_int, anything else is an error.
- `function to_float(string s): f64!ParseError` — A whole string as a float: [sign] digits [. digits] [e [sign] digits].

<a id="stdmodule-fmt"></a>
## std/fmt

Formatting numbers and text.

```watr
import "std/fmt";
string a = fmt::format("% + % = %", [1, 2, 3]);   // "1 + 2 = 3"
string b = fmt::fixed(3.14159, 2);                // "3.14"
string c = fmt::pad_left("7", 3, '0');            // "007"
```

- `function pad_left(string s, usize width, u8 fill): string` — s padded on the left with `fill` up to `width` bytes.
- `function pad_right(string s, usize width, u8 fill): string` — s padded on the right with `fill` up to `width` bytes.
- `function center(string s, usize width, u8 fill): string` — s centered in `width` bytes.
- `function hex(u64 v, usize width): string` — "ff" for 255; at least `width` digits.
- `function bin(u64 v, usize width): string` — "1010" for 10; at least `width` digits.
- `function oct(u64 v, usize width): string`
- `function fixed(f64 x, int decimals): string` — x with exactly `decimals` digits after the point, rounded half away from zero: fixed(2.345, 2) is "2.35" (as far as floats allow).
- `function grouped(i64 v): string` — 1234567 -> "1,234,567"
- `function bytes(u64 n): string` — A byte count for people: "1.5 KiB", "3.0 MiB".
- `function format(string pattern, array args): string` — Replaces each `%` in `pattern` with the next value from `args`, as text (`{}` would be interpolation). `%%` stands for a literal `%`.

<a id="stdmodule-math"></a>
## std/math

Numbers. The f64 functions beyond the basics come from the C math
library, which is linked automatically when they're used.

- `const f64 PI = 3.14159265358979323846`
- `const f64 E = 2.71828182845904523536`
- `function abs(int x): int`
- `function min(int a, int b): int`
- `function max(int a, int b): int`
- `function clamp(int x, int lo, int hi): int`
- `function fabs(f64 x): f64`
- `function fmin(f64 a, f64 b): f64`
- `function fmax(f64 a, f64 b): f64`
- `function gcd(int a, int b): int` — Greatest common divisor.
- `function ipow(int x, int n): int` — x to the power n, by squaring (n >= 0).

<a id="stdmodule-vec"></a>
## std/vec

Vec<T>: a growable array. Elements live in one block of garbage-collected
memory that doubles as needed, so push is O(1) on average and get/set
are O(1). Indexes are bounds-checked. Loop over one with foreach:

```watr
import "std/vec";
vec::Vec<int> v = new vec::Vec<int>();
v->push(1);
foreach (v as x) { echo x; }
```

### `class Vec<T>`

  - `public function reserve(int count): void` — Room for `count` elements without growing again.
  - `public function len(): int`
  - `public function is_empty(): bool`
  - `public function get(int i): T`
  - `public function set(int i, T v): void`
  - `public function push(T v): void`
  - `public function pop(): T`
  - `public function first(): T`
  - `public function last(): T`
  - `public function insert(int i, T v): void` — Inserts v before index i (i == len() appends).
  - `public function remove(int i): T` — Removes and returns the element at i, shifting the rest down.
  - `public function swap(int i, int j): void`
  - `public function clear(): void`
  - `public function truncate(int count): void` — Keeps the first `count` elements.
  - `public function index_of(T v): int` — Where v first appears (compared with ==), or -1.
  - `public function contains(T v): bool`
  - `public function reverse(): void`
  - `public function slice(int start, int end): Vec<T>` — A new Vec with elements [start, end).
  - `public function copy(): Vec<T>`
  - `public function extend(Vec<T> other): void` — Appends every element of `other`.
  - `public function join(string sep): string` — The elements as text, joined by `sep`.
  - `public function data(): T*` — The storage, for passing to code that takes a pointer. Valid until the Vec grows.

- `function of<T>(T* items, int count): Vec<T>` — A Vec holding the given fixed array's elements.

<a id="stdmodule-map"></a>
## std/map

Map<K, V>: a hash map. Keys can be anything `hash_of` and `==` accept:
numbers, bools, enums, strings, pointers, objects (by identity). Entries
are kept in insertion order, like PHP arrays, and a table of entry
numbers (open addressing, linear probing) finds keys.

```watr
import "std/map";
map::Map<string, int> ages = new map::Map<string, int>();
ages->set("ada", 36);
int a = ages->get("ada") ?? 0;
foreach (ages->keys() as k) { echo k, "\n"; }
```

- `enum MapError { Missing }`
### `class Map<K, V>`

  - `public function len(): int`
  - `public function is_empty(): bool`
  - `public function set(K key, V value): void` — Adds key => value, or replaces the value if the key is there.
  - `public function has(K key): bool`
  - `public function get(K key): V!MapError` — The value for `key`, or MapError::Missing.
  - `public function get_or(K key, V fallback): V` — The value for `key`, or `fallback`.
  - `public function remove(K key): bool` — Removes `key`; false if it wasn't there.
  - `public function clear(): void`
  - `public function keys(): vec::Vec<K>` — The keys, in insertion order.
  - `public function values(): vec::Vec<V>` — The values, in insertion order.


<a id="stdmodule-sort"></a>
## std/sort

Sorting and searching. Elements are compared with `<` (numbers, strings,
enums...), or by a Less<T> object for anything else. Sorting is stable:
equal elements keep their order (a merge sort, with insertion sort for
short runs).

```watr
import "std/sort";
import "std/vec";
vec::Vec<int> scores = new vec::Vec<int>();
scores->push(3);
scores->push(1);
sort::vec(scores);                    // [1, 3]
// sort::vec_by(people, new ByAge()) with `class ByAge implements sort::Less<Person>`
```

### `interface Less<T>`

An ordering for sort::by and friends.

  - `function less(T a, T b): bool`

- `function slice<T>(T* xs, int n): void` — Sorts xs[0..n) in place, smallest first.
- `function vec<T>(vec::Vec<T> v): void` — Sorts a Vec in place, smallest first.
- `function slice_by<T>(T* xs, int n, Less<T> cmp): void` — Sorts xs[0..n) in place by `cmp`.
- `function vec_by<T>(vec::Vec<T> v, Less<T> cmp): void` — Sorts a Vec in place by `cmp`.
- `function is_sorted<T>(vec::Vec<T> v): bool`
- `function search<T>(vec::Vec<T> v, T x): int` — In a sorted Vec: an index holding x, or -1.
- `function min<T>(vec::Vec<T> v): T`
- `function max<T>(vec::Vec<T> v): T`

<a id="stdmodule-arrays"></a>
## std/arrays

Helpers for PHP-style arrays (ordered maps from int or string keys to
mixed values). Arrays are shared, not copied, when assigned or passed;
copy() makes an independent one.

- `function has(array a, mixed key): bool` — Is there an element with this key?
- `function remove(array a, mixed key): bool` — Removes the element with this key; false if there wasn't one.
- `function push(array a, mixed v): void` — Appends, like `a[] = v`.
- `function pop(array a): mixed` — Removes and returns the last element (null if the array is empty).
- `function keys(array a): array` — The keys, in order, as a list.
- `function values(array a): array` — The values, in order, as a list.
- `function contains(array a, mixed v): bool` — Is some element equal to v (same type and value)?
- `function key_of(array a, mixed v): mixed` — The key of the first element equal to v, or null.
- `function join(array a, string sep): string` — The elements as text, separated by sep.
- `function range(int lo, int hi): array` — [lo, lo + 1, ..., hi]
- `function copy(array a): array` — A new array with the same keys and values.
- `function merge(array a, array b): array` — Like PHP's array_merge: b's string keys replace a's; int keys are renumbered from 0.
- `function reverse(array a): array` — The values in reverse order, as a list.
- `function type_of(mixed v): string` — The type of value a mixed holds: "null", "bool", "int", "float", "string", "array" or "object".

<a id="stdmodule-fs"></a>
## std/fs

Files, through Linux system calls. Failures come back as IoError values:

```watr
import "std/fs";
string text = fs::read("notes.txt") ?? "";   // or `?` to pass the error on
bool saved = fs::write("out.txt", "hello\n") ?? false;
```

- `enum IoError { NotFound, Denied, Exists, IsDir, NotDir, NotEmpty, Other }`
- `function error_of(i64 r): IoError` — The IoError for a negative syscall result (-errno).
- `function describe(IoError e): string` — A sentence describing e, for messages.
- `function read(string path): string!IoError` — The whole file as a string.
- `function write(string path, string data): bool!IoError` — Replaces the file's contents (creating it if needed).
- `function append(string path, string data): bool!IoError` — Adds to the end of the file (creating it if needed).
- `function exists(string path): bool`
- `function remove(string path): bool!IoError`
- `function rename(string from, string to): bool!IoError`
- `function mkdir(string path): bool!IoError`
- `function rmdir(string path): bool!IoError`
- `function size(string path): i64!IoError` — The file's size in bytes.

<a id="stdmodule-os"></a>
## std/os

The process: command-line arguments, environment variables, exiting.

- `function args(): vec::Vec<string>` — The command line: args()[0] is the program, then its arguments.
- `function argc(): int` — How many arguments there are, not counting the program.
- `function getenv(string name): string` — The environment variable `name`, or "" if it isn't set.
- `function has_env(string name): bool`
- `function exit(int code): void` — Ends the program now, with exit code `code`. (Deferred code doesn't run.)
- `function pid(): int`

<a id="stdmodule-time"></a>
## std/time

Clocks and sleeping, through Linux system calls.

- `function now_ns(): i64` — Nanoseconds on a clock that only goes forward (for measuring).
- `function elapsed_ms(i64 start_ns): f64` — Milliseconds since `start_ns` (a value from now_ns()).
- `function unix(): i64` — Seconds since 1970-01-01 UTC.
- `function unix_ms(): i64` — Milliseconds since 1970-01-01 UTC.
- `function sleep_ns(i64 ns): void`
- `function sleep_ms(int ms): void`
- `struct DateTime { int year; int month; int day; int hour; int minute; int second; }` — A civil date and time, UTC.
- `function utc(i64 t): DateTime` — Splits Unix time into a date (Howard Hinnant's days-to-civil algorithm).
- `function format(DateTime d): string` — "2026-10-13 14:05:09"

<a id="stdmodule-rand"></a>
## std/rand

Pseudo-random numbers: xoshiro256** seeded through splitmix64. Not for
cryptography. Rng objects give repeatable sequences from a seed; the
module-level functions use one seeded from the kernel's randomness.

### `class Rng`

  - `public function reseed(u64 seed): void`
  - `public function next(): u64` — Any 64-bit value.
  - `public function range(int lo, int hi): int` — A whole number in [lo, hi], inclusive.
  - `public function unit(): f64` — A float in [0, 1).
  - `public function chance(f64 p): bool`

- `function default_rng(): Rng` — The module's generator, seeded from getrandom() the first time.
- `function seed(u64 s): void`
- `function range(int lo, int hi): int`
- `function unit(): f64`
- `function chance(f64 p): bool`
- `function shuffle<T>(vec::Vec<T> v, Rng r): void` — Shuffles a Vec in place (Fisher-Yates).
- `function pick<T>(vec::Vec<T> v, Rng r): T` — A random element.

<a id="stdmodule-mem"></a>
## std/mem

Raw memory.

- `function copy(u8* dst, const u8* src, usize n): void`
- `function set(u8* dst, u8 value, usize n): void`
- `function equal(const u8* a, const u8* b, usize n): bool`

<a id="stdmodule-gc"></a>
## std/gc

The garbage collector: objects from `new` and strings built at run time
are freed automatically once nothing refers to them.

- `function collect(): void` — Runs a collection now.
- `function live_bytes(): usize` — Bytes in objects that survived the last collection.
- `function heap_bytes(): usize` — Bytes the heap has taken from the operating system.
- `function collections(): usize` — How many collections have run.

<a id="stdmodule-sys"></a>
## std/sys

Linux system calls, made directly with the `syscall` instruction (no C
library needed). Each returns the kernel's result: a value >= 0, or
-errno on failure. std/fs, std/os and std/time are built on these.

- `function syscall0(i64 n): i64`
- `function syscall1(i64 n, i64 a): i64`
- `function syscall2(i64 n, i64 a, i64 b): i64`
- `function syscall3(i64 n, i64 a, i64 b, i64 c): i64`
- `function syscall4(i64 n, i64 a, i64 b, i64 c, i64 d): i64`
- `function syscall6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f): i64`
- `const i64 READ = 0` — Syscall numbers (x86-64).
- `const i64 WRITE = 1`
- `const i64 OPEN = 2`
- `const i64 CLOSE = 3`
- `const i64 LSEEK = 8`
- `const i64 NANOSLEEP = 35`
- `const i64 GETPID = 39`
- `const i64 EXIT_GROUP = 231`
- `const i64 RENAME = 82`
- `const i64 MKDIR = 83`
- `const i64 RMDIR = 84`
- `const i64 UNLINK = 87`
- `const i64 ACCESS = 21`
- `const i64 CLOCK_GETTIME = 228`
- `const i64 GETRANDOM = 318`
- `function cstr(string s): const u8*` — A NUL-terminated copy of s, for passing to the kernel.
