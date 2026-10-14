# watrlang standard library

Generated from the sources by `tools/gen_stdlib_doc.py`; don't edit by hand.
Import a module with `import "std/NAME";` and call its functions as
`NAME::function(...)` (or unqualified, when no other import uses the name).

Modules: [`std/io`](#stdmodule-io), [`std/str`](#stdmodule-str), [`std/fmt`](#stdmodule-fmt), [`std/math`](#stdmodule-math), [`std/vec`](#stdmodule-vec), [`std/map`](#stdmodule-map), [`std/sort`](#stdmodule-sort), [`std/arrays`](#stdmodule-arrays), [`std/fs`](#stdmodule-fs), [`std/os`](#stdmodule-os), [`std/time`](#stdmodule-time), [`std/rand`](#stdmodule-rand), [`std/json`](#stdmodule-json), [`std/hash`](#stdmodule-hash), [`std/crypto`](#stdmodule-crypto), [`std/password`](#stdmodule-password), [`std/mem`](#stdmodule-mem), [`std/gc`](#stdmodule-gc), [`std/sys`](#stdmodule-sys).

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

Files, on Linux and Windows. Failures come back as IoError values:

```watr
import "std/fs";
string text = fs::read("notes.txt") ?? "";   // or `?` to pass the error on
bool saved = fs::write("out.txt", "hello\n") ?? false;
```

- `enum IoError { NotFound, Denied, Exists, IsDir, NotDir, NotEmpty, Other }`
- `function error_of(i64 r): IoError` — The IoError for a negative result (-errno, as std/platform returns).
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

Clocks and sleeping.

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
cryptography: use std/crypto for keys, tokens and anything secret.
Rng objects give repeatable sequences from a seed; the module-level
functions use one seeded from the operating system's randomness.

### `class Rng`

  - `public function reseed(u64 seed): void`
  - `public function next(): u64` — Any 64-bit value.
  - `public function range(int lo, int hi): int` — A whole number in [lo, hi], inclusive.
  - `public function unit(): f64` — A float in [0, 1).
  - `public function chance(f64 p): bool`

- `function default_rng(): Rng` — The module's generator, seeded from the OS's random generator the first time.
- `function seed(u64 s): void`
- `function range(int lo, int hi): int`
- `function unit(): f64`
- `function chance(f64 p): bool`
- `function shuffle<T>(vec::Vec<T> v, Rng r): void` — Shuffles a Vec in place (Fisher-Yates).
- `function pick<T>(vec::Vec<T> v, Rng r): T` — A random element.

<a id="stdmodule-json"></a>
## std/json

JSON, like PHP's json_encode/json_decode, for PHP-style arrays and mixed:

```watr
import "std/json";
string text = json::encode(["ok" => true, "items" => [1, 2, 3], "user" => ["name" => "ada"]]);
// {"ok":true,"items":[1,2,3],"user":{"name":"ada"}}
mixed data = json::decode("\{\"n\": 5\}") ?? null;   // \{ is a literal brace
```

encode: arrays with keys 0, 1, 2... become JSON arrays, other arrays
become objects (an empty array is []). Strings are written as UTF-8
(invalid bytes become U+FFFD), and < > & are escaped as < >
& so the output is safe inside an HTML <script>. Floats use the
shortest form that reads back exactly and keep a ".0" (so 1.0 stays a
float); NaN and infinity become null, as do objects.
decode: objects become arrays with string keys, arrays become lists,
numbers become int (or float when they have a fraction, an exponent or
don't fit in 64 bits). Input must be strict JSON (RFC 8259).

- `enum JsonError { Syntax, Depth }`
- `function encode(mixed value): string` — Compact JSON for any value: arrays (nested), strings, ints, floats, bools and null.
- `function pretty(mixed value): string` — Like encode, indented by 4 spaces (PHP's JSON_PRETTY_PRINT).

<a id="stdmodule-hash"></a>
## std/hash

Cryptographic hashes and what's built on them: SHA-256, SHA-512, SHA-1,
MD5, BLAKE2b, HMAC, HKDF and PBKDF2. Everything takes and returns byte
strings; turn a digest into text with crypto::hex or crypto::base64.

```watr
import "std/hash";
import "std/crypto";
echo crypto::hex(hash::sha256("abc")), "\n";
```

For passwords use std/password (Argon2id), not these directly.
SHA-1 and MD5 are broken for security; they're here for old protocols
and checksums only.

- `function sha256(string data): string` — SHA-256: 32 bytes.
- `function sha512(string data): string` — SHA-512: 64 bytes.
- `function sha1(string data): string` — SHA-1: 20 bytes. Broken for security; only for old protocols.
- `function md5(string data): string` — MD5: 16 bytes. Broken for security; only for checksums and old protocols.
- `function blake2b(string data, usize out_len, string key): string` — BLAKE2b with an output of 1-64 bytes and an optional key (0-64 bytes; "" for none). The same as libsodium's crypto_generichash.
- `function hmac_sha256(string key, string message): string` — HMAC-SHA-256: 32 bytes.
- `function hmac_sha512(string key, string message): string` — HMAC-SHA-512: 64 bytes.
- `function hmac_sha1(string key, string message): string` — HMAC-SHA-1: 20 bytes (legacy protocols, TOTP).
- `function hkdf_sha256(string ikm, string salt, string info, usize length): string` — Derives `length` bytes (at most 8160) from input key material. salt may be ""; info separates keys for different purposes.
- `function hkdf_sha512(string ikm, string salt, string info, usize length): string` — HKDF with SHA-512 (at most 16320 bytes).
- `function pbkdf2_sha256(string password, string salt, int iterations, usize length): string` — PBKDF2-HMAC-SHA-256. OWASP suggests 600000 iterations for passwords, but prefer std/password (Argon2id) for new password storage.
- `function pbkdf2_sha512(string password, string salt, int iterations, usize length): string` — PBKDF2-HMAC-SHA-512 (OWASP: 210000 iterations).

<a id="stdmodule-crypto"></a>
## std/crypto

Secure randomness, constant-time comparison, hex and base64, authenticated
encryption (XChaCha20-Poly1305, as in libsodium) and signed tokens.

```watr
import "std/crypto";
string key = crypto::new_key();                 // keep it secret, 32 bytes
string box = crypto::seal(key, "card 4242");    // encrypted and authenticated
string back = crypto::open(key, box) ?? "";     // "" if tampered with
string token = crypto::sign(key, "user=42", 3600);   // valid for an hour
string who = crypto::unsign(key, token) ?? "";
```

Everything works on byte strings. Randomness comes from the operating
system (getrandom on Linux, RtlGenRandom on Windows), never from std/rand.

- `enum CryptoError { Invalid, Forged, Expired }`
- `function random_bytes(usize n): string` — n bytes from the OS's cryptographic random generator.
- `function random_int(int lo, int hi): int` — A uniformly random integer in [lo, hi], inclusive, without modulo bias.
- `function random_token(usize bytes): string` — A random URL-safe token carrying `bytes` bytes of randomness (32 is a good size for session ids and reset links).
- `function equal(string a, string b): bool` — True if a and b are equal. Takes the same time wherever they differ, so it's safe for comparing MACs, tokens and hashes.
- `function hex(string data): string` — Lower-case hex.
- `function unhex(string text): string!CryptoError`
- `function base64(string data): string` — Standard base64 with '=' padding.
- `function unbase64(string text): string!CryptoError`
- `function base64url(string data): string` — URL- and filename-safe base64 (- and _), without padding.
- `function unbase64url(string text): string!CryptoError`
- `function chacha20(string key, string nonce, u32 counter, string data): string` — XORs data with the ChaCha20 keystream (32-byte key, 12-byte nonce). Encryption and decryption are the same operation. Unauthenticated: use seal()/aead_encrypt() unless you're building a protocol.
- `function poly1305(string key, string msg): string` — A 16-byte one-time authenticator. The 32-byte key must never be reused.
- `function aead_encrypt(string key, string nonce, string plaintext, string aad): string` — ChaCha20-Poly1305 (RFC 8439, the IETF variant): ciphertext followed by a 16-byte tag. Never encrypt two messages with the same key and nonce.
- `function aead_decrypt(string key, string nonce, string sealed, string aad): string!CryptoError`
- `function xaead_encrypt(string key, string nonce, string plaintext, string aad): string` — XChaCha20-Poly1305 with a 24-byte nonce (random nonces are safe). Matches libsodium's crypto_aead_xchacha20poly1305_ietf_*.
- `function xaead_decrypt(string key, string nonce, string sealed, string aad): string!CryptoError`
- `function new_key(): string` — A fresh random 32-byte key for seal() and sign().
- `function seal(string key, string plaintext): string` — Encrypts and authenticates with a random nonce: the result is nonce (24) + ciphertext + tag (16), 40 bytes longer than the message. Store it as is, or base64url() it for cookies and URLs.
- `function open(string key, string sealed): string!CryptoError` — Undoes seal(); Forged if the data was altered or the key is wrong.
- `function sign(string key, string message, int seconds): string` — A tamper-proof (not secret) token carrying `message`, valid for `seconds` (0 = forever): for cookies, password-reset and email links. Format: base64url(message).expiry.base64url(HMAC-SHA-256).
- `function unsign(string key, string token): string!CryptoError` — The message inside a token from sign(), if its signature is valid and it hasn't expired.

<a id="stdmodule-password"></a>
## std/password

Password hashing, compatible with PHP's password_hash/password_verify:

```watr
import "std/password";
string stored = password::hash("correct horse");          // Argon2id
if (password::verify("correct horse", stored)) {
    echo "welcome back\n";
}
if (password::needs_rehash(stored)) {
    stored = password::hash("correct horse");   // then save it
}
```

password::hash makes Argon2id hashes in the standard PHC format
($argon2id$v=19$m=...,t=...,p=...$salt$hash), the same strings PHP,
libsodium, Python's argon2-cffi and others produce and accept.
password::verify also accepts bcrypt ($2y$, $2b$, $2a$, as made by PHP's
PASSWORD_BCRYPT and most frameworks) and PBKDF2 ($pbkdf2-sha256$ from
passlib, pbkdf2_sha256$ from Django), so users can log in after moving
from another system and be upgraded with needs_rehash.

- `const int MEMORY_KIB = 19456` — Argon2id cost: 19 MiB of memory and 2 passes, OWASP's recommended minimum. Raise MEMORY_KIB if logins can afford more time.
- `const int TIME_COST = 2`
- `const int LANES = 1`
- `const int BCRYPT_COST = 12`
- `const int ARGON2D = 0`
- `const int ARGON2I = 1`
- `const int ARGON2ID = 2`
- `function argon2(int kind, string password, string salt, int t, int m, int p, usize out_len, int version): string` — Raw Argon2: `out_len` bytes from a password and salt (at least 8 bytes). kind is ARGON2ID (use this), ARGON2I or ARGON2D; t passes over m KiB of memory with p lanes. version is 0x13 (current) or 0x10. Returns "" for parameters out of range.
- `function hash_argon2id(string password, int memory_kib, int time_cost, int lanes): string` — An Argon2id hash with chosen costs (memory in KiB).
- `function hash(string password): string` — The recommended way to store a password: Argon2id with the costs above.
- `function derive_key(string password, string salt): string` — A 32-byte encryption key from a password (Argon2id), for use with crypto::seal. Store the salt (16 random bytes) next to the data.
### `class Argon2Hash`

The parts of a $argon2...$ string (see parse_argon2).


- `function parse_argon2(string stored): Argon2Hash` — Splits a $argon2...$ string.
- `function hash_bcrypt(string password, int cost): string` — A bcrypt hash ($2y$, as PHP's PASSWORD_BCRYPT makes) with cost 4-31. bcrypt only uses the first 72 bytes of a password and can't take NUL bytes; prefer password::hash (Argon2id) unless you need bcrypt.
- `function verify(string password, string stored): bool` — Checks a password against any hash this module understands, in constant time. False for a wrong password or an unknown format.
- `function needs_rehash(string stored): bool` — True if `stored` isn't an Argon2id hash at least as strong as what password::hash makes today: after a successful login, hash the password again and save the new hash.

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
-errno on failure. Linux only: the portable modules (std/fs, std/os,
std/time) go through std/platform, which uses these on Linux.

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
