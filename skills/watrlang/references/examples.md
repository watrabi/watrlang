# Example watrlang programs

Complete, idiomatic programs that compile with `watrc`. Use them as models
for structure and style.

## Contents

1. [Word frequency from a file (args, files, maps, sorting)](#1-word-frequency)
2. [Bank accounts (classes, inheritance, error unions)](#2-bank-accounts)
3. [Generic containers and algorithms](#3-generic-containers)
4. [Low-level: a byte buffer with manual memory](#4-a-byte-buffer)
5. [Parsing with error propagation](#5-parsing)
6. [Calling C](#6-calling-c)

## 1. Word frequency

Reads the file named on the command line (or a built-in text), counts words,
and prints the most common ones.

```watr
import "std/fs";
import "std/os";
import "std/str";
import "std/vec";
import "std/map";
import "std/sort";
import "std/fmt";

class Entry {
    public string word;
    public int count;

    public function __construct(string word, int count) {
        this->word = word;
        this->count = count;
    }
}

// Most frequent first; ties alphabetically.
class ByCount implements sort::Less<Entry> {
    public function less(Entry a, Entry b): bool {
        if (a->count != b->count) {
            return a->count > b->count;
        }
        return a->word < b->word;
    }
}

function load_text(): string {
    vec::Vec<string> args = os::args();
    if (args->len() < 2) {
        return "the cat and the hat and the bat";
    }
    string!fs::IoError text = fs::read(args->get(1));
    if (!text->ok) {
        echo "can't read ", args->get(1), ": ", fs::describe(text->error), "\n";
        os::exit(1);
    }
    return text->value;
}

function main(): int {
    string text = str::to_lower(load_text());
    map::Map<string, int> counts = new map::Map<string, int>();
    foreach (str::words(text) as w) {
        counts->set(w, counts->get_or(w, 0) + 1);
    }

    vec::Vec<Entry> entries = new vec::Vec<Entry>();
    foreach (counts->keys() as w) {
        entries->push(new Entry(w, counts->get(w) ?? 0));
    }
    sort::vec_by(entries, new ByCount());

    int shown = 0;
    foreach (entries as e) {
        if (shown == 3) {
            break;
        }
        echo fmt::pad_right(e->word, 8, ' '), fmt::pad_left("{e->count}", 4, ' '), "\n";
        shown++;
    }
    return 0;
}
```

## 2. Bank accounts

```watr
import "std/vec";
import "std/fmt";

enum TxError { InsufficientFunds, InvalidAmount, Frozen }

abstract class Account {
    protected string owner;
    protected int cents;
    protected bool frozen = false;

    public function __construct(string owner, int cents) {
        this->owner = owner;
        this->cents = cents;
    }

    // Each kind of account decides how far it may go below zero.
    abstract public function limit(): int;

    public function withdraw(int amount): int!TxError {
        if (amount <= 0) {
            return TxError::InvalidAmount;
        }
        if (this->frozen) {
            return TxError::Frozen;
        }
        if (this->cents - amount < -this->limit()) {
            return TxError::InsufficientFunds;
        }
        this->cents -= amount;
        return this->cents;
    }

    public function deposit(int amount): void!TxError {
        if (amount <= 0) {
            return TxError::InvalidAmount;
        }
        this->cents += amount;
    }

    public function freeze(): void { this->frozen = true; }

    public function __toString(): string {
        return this->owner . ": " . fmt::fixed(this->cents as f64 / 100.0, 2);
    }
}

class Checking extends Account {
    public function limit(): int { return 5000; }   // $50 overdraft
}

class Savings extends Account {
    public function limit(): int { return 0; }
}

// Moves money, undoing the withdrawal if the deposit fails.
function transfer(Account from, Account to, int amount): void!TxError {
    from->withdraw(amount)?;
    void!TxError d = to->deposit(amount);
    if (!d->ok) {
        if (!from->deposit(amount)->ok) {   // `??` can't be used on a void!E
            echo "rollback failed\n";
        }
        return d->error;
    }
}

function main(): int {
    vec::Vec<Account> all = new vec::Vec<Account>();
    Account a = new Checking("ada", 10000);
    Account b = new Savings("bob", 2500);
    all->push(a);
    all->push(b);

    void!TxError t1 = transfer(a, b, 12000);
    echo "transfer 1: ", t1->ok ? "ok" : "failed", "\n";
    void!TxError t2 = transfer(b, a, 99999);
    if (!t2->ok) {
        echo "transfer 2 failed: ", t2->error, "\n";
    }
    b->freeze();
    echo "withdraw from frozen: ", b->withdraw(100) ?? -1, "\n";
    foreach (all as acct) {
        echo acct, "\n";
    }
    return 0;
}
```

## 3. Generic containers

```watr
import "std/vec";

// A fixed-capacity ring buffer.
class Ring<T> {
    private T[8] items;
    private int head;
    private int count;

    public function push(T v): bool {
        if (this->count == 8) {
            return false;
        }
        this->items[(this->head + this->count) % 8] = v;
        this->count++;
        return true;
    }

    public function pop(): T {
        T v = this->items[this->head];
        this->head = (this->head + 1) % 8;
        this->count--;
        return v;
    }

    public function len(): int { return this->count; }
    public function get(int i): T { return this->items[(this->head + i) % 8]; }   // makes foreach work
}

struct Pair<A, B> {
    A first;
    B second;
}

function zip<A, B>(vec::Vec<A> xs, vec::Vec<B> ys): vec::Vec<Pair<A, B>> {
    vec::Vec<Pair<A, B>> out = new vec::Vec<Pair<A, B>>();
    int n = xs->len() < ys->len() ? xs->len() : ys->len();
    for (int i = 0; i < n; i++) {
        out->push(Pair<A, B> { first: xs->get(i), second: ys->get(i) });
    }
    return out;
}

function count_if_above<T>(vec::Vec<T> v, T limit): int {
    int n = 0;
    foreach (v as x) {
        if (x > limit) {
            n++;
        }
    }
    return n;
}

function main(): int {
    Ring<string> r = new Ring<string>();
    r->push("a");
    r->push("b");
    r->push("c");
    echo r->pop(), " then";
    foreach (r as s) {
        echo " ", s;
    }
    echo "\n";

    vec::Vec<string> names = new vec::Vec<string>();
    names->push("x");
    names->push("y");
    vec::Vec<f64> scores = new vec::Vec<f64>();
    scores->push(1.5);
    scores->push(9.0);
    scores->push(4.0);
    foreach (zip(names, scores) as p) {
        echo p->first, "=", p->second, " ";
    }
    echo "\n", count_if_above(scores, 2.0), "\n";
    return 0;
}
```

## 4. A byte buffer

Manual memory with `alloc`/`free`, pointers, and `defer`.

```watr
struct Buffer {
    u8* data;
    usize len;
    usize cap;
}

function buffer_new(usize cap): Buffer {
    return Buffer { data: alloc(u8, cap), len: 0, cap: cap };
}

function buffer_push(Buffer* b, u8 byte): void {
    if (b->len == b->cap) {
        usize bigger = b->cap * 2;
        u8* grown = alloc(u8, bigger);
        for (usize i = 0; i < b->len; i++) {
            grown[i] = b->data[i];
        }
        free(b->data);
        b->data = grown;
        b->cap = bigger;
    }
    b->data[b->len] = byte;
    b->len++;
}

function buffer_append(Buffer* b, string s): void {
    for (usize i = 0; i < len(s); i++) {
        buffer_push(b, s[i]);
    }
}

// A string that shares the buffer's bytes (valid until it changes).
function buffer_view(Buffer* b): string {
    return string { ptr: b->data, len: b->len };
}

function checksum(Buffer* b): u32 {
    u32 sum = 0;
    for (usize i = 0; i < b->len; i++) {
        sum = (sum << 5) + sum + b->data[i] as u32;   // wraps, like C
    }
    return sum;
}

function main(): int {
    Buffer b = buffer_new(4);
    defer free(b->data);
    buffer_append(&b, "hello, ");
    buffer_append(&b, "buffer");
    echo buffer_view(&b), " (", b->len, " bytes, cap ", b->cap, ") ", checksum(&b), "\n";
    return 0;
}
```

## 5. Parsing

Error unions with `?` propagation through several layers.

```watr
import "std/str";
import "std/vec";

enum ConfigError { MissingEquals, BadNumber, UnknownKey }

struct Config {
    int width;
    int height;
    int depth;
}

function parse_number(string s): int!ConfigError {
    int!str::ParseError n = str::to_int(s);
    if (!n->ok) {
        return ConfigError::BadNumber;
    }
    return n->value;
}

function apply(Config* c, string line): void!ConfigError {
    int eq = str::index_of(line, "=");
    if (eq < 0) {
        return ConfigError::MissingEquals;
    }
    string key = str::trim(str::substr(line, 0, eq as usize));
    string value = str::trim(str::substr(line, (eq + 1) as usize, len(line)));
    int n = parse_number(value)?;
    if (key == "width") {
        c->width = n;
    } else if (key == "height") {
        c->height = n;
    } else if (key == "depth") {
        c->depth = n;
    } else {
        return ConfigError::UnknownKey;
    }
}

function parse(string text): Config!ConfigError {
    Config c = Config { width: 80, height: 24 };
    foreach (str::lines(text) as line) {
        if (len(str::trim(line)) == 0) {
            continue;
        }
        apply(&c, line)?;
    }
    return c;
}

function main(): int {
    Config!ConfigError good = parse("width = 120\nheight=40\n");
    if (good->ok) {
        Config c = good->value;
        echo c->width, "x", c->height, " depth ", c->depth, "\n";
    }
    Config!ConfigError bad = parse("width = 120\ncolor = 3\n");
    if (!bad->ok) {
        echo "error: ", bad->error, "\n";       // error: UnknownKey
    }
    return 0;
}
```

## 6. Calling C

```watr
extern function printf(const u8* fmt, ...): i32;
extern function getpid(): i32;

export function watr_version(): int {        // C code could call this
    return 19;
}

function main(): int {
    printf("pid > 0: %d, version %d\n", getpid() > 0 ? 1 : 0, watr_version());
    string s = "C sees a NUL-terminated string";
    printf("%s (%zu bytes)\n", s->ptr, len(s));
    return 0;
}
```
