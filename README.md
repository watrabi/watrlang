# watrlang

A compiled systems language with PHP-flavored syntax, written in plain C.
It's low-level enough to write an operating system in.

```php
import "std/io";

function main(): int {
    string name = "world";
    echo "Hello, {name}!\n";
    return 0;
}
```

The full plan is in [docs/DESIGN.md](docs/DESIGN.md).

## Status

| Milestone | State |
|-----------|-------|
| 0. Skeleton (build, arena, diagnostics) | done |
| 1. Lexer | done |
| 2. Parser | done |
| 3. First binary (codegen) | next |
| 4+. Types, FFI, strings, ... | planned |

## Build

Needs a C99 compiler and `make`.

```sh
make                          # builds build/watrc
make test                     # runs the test suite
make SAN=1 CC=gcc test        # same, with AddressSanitizer + UBSan
./build/watrc --tokens file.watr   # dump tokens
./build/watrc --ast file.watr      # dump the syntax tree
```

## Tests

Each `tests/lexer/*.watr` (token dump) and `tests/parser/*.watr` (syntax
tree) has a `.expected` file holding the output, error messages and exit code. After an intentional output change, regenerate
them with `UPDATE=1 ./tests/run_tests.sh` and review the diff.
